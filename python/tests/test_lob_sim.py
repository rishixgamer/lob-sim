import gc
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

import numpy as np

from lob_sim import Action, ActionKind, BookEnv, Event, MatchingEngine, MsgType
from lob_sim import ReplayError, ReplayFormat, Side, Status


FIXTURES = Path(__file__).resolve().parents[2] / "fixtures" / "continuous"


class BindingTests(unittest.TestCase):
    def test_native_parity_and_replay_callback_lifetime(self):
        rows = [
            (MsgType.New, 0, 0, 5, 101, Side.Sell),
            (MsgType.New, 1, 0, 3, 101, Side.Sell),
            (MsgType.New, 2, 1, 4, 102, Side.Sell),
            (MsgType.New, 3, 2, 10, 102, Side.Buy),
            (MsgType.New, 4, 3, 4, 99, Side.Buy),
            (MsgType.Modify, 4, 4, 4, 102),
            (MsgType.Market, 5, 5, 1, None, Side.Sell),
            (MsgType.Cancel, 4, 6, 3),
            (MsgType.Cancel, 5, 7, 1),
            (MsgType.Market, 6, 8, 2, None, Side.Buy),
            (MsgType.New, 7, 9, 6, 98, Side.Buy),
            (MsgType.Modify, 7, 9, 4, 98),
        ]
        direct = MatchingEngine()
        expected = [direct.submit(Event.create(*row)) for row in rows]
        self.assertEqual([(t.maker_id, t.taker_id, t.price_ticks, t.qty) for t in expected[3].fills],
                         [(0, 3, 101, 5), (1, 3, 101, 3), (2, 3, 102, 2)])
        self.assertEqual(expected[7].status, Status.InvalidQuantity)
        self.assertEqual(expected[8].status, Status.AlreadyClosed)
        self.assertEqual(expected[9].canceled, 2)

        for extension, fmt in (("csv", ReplayFormat.Csv), ("bin", ReplayFormat.Binary)):
            with self.subTest(extension=extension):
                engine = MatchingEngine()
                retained = []
                report = engine.replay(str(FIXTURES / f"session.{extension}"), fmt,
                                       lambda event, result: retained.append((event, result)))
                self.assertIsNone(report.diagnostic)
                self.assertEqual(report.events, len(rows))
                self.assertEqual([res for _, res in retained], expected)
                self.assertEqual([event.order_id for event, _ in retained], [row[1] for row in rows])
                self.assertEqual(engine.depth(Side.Buy, 10), direct.depth(Side.Buy, 10))
                self.assertEqual(engine.queue(Side.Buy, 102), [4])
                self.assertEqual(engine.queue_position(7), direct.queue_position(7))
                self.assertEqual(engine.depth(Side.Sell, 10), [])

    def test_checked_integer_fields_and_replay_diagnostics(self):
        upper = 2**64
        for raw in (-1, upper, 1.5, True, None):
            self.assertEqual(Event.create(MsgType.New, raw, 1, 1, 100, Side.Buy),
                             Status.InvalidOrderId)
            self.assertEqual(Event.create(MsgType.New, 1, raw, 1, 100, Side.Buy),
                             Status.InvalidTimestamp)
            self.assertEqual(Event.create(MsgType.New, 1, 1, raw, 100, Side.Buy),
                             Status.InvalidQuantity)
        for raw in (2**63, -(2**63) - 1, -1, 0, 1.5, None):
            self.assertEqual(Event.create(MsgType.New, 1, 1, 1, raw, Side.Buy),
                             Status.InvalidPrice)
        event = Event.create(MsgType.New, np.uint64(upper - 1), upper - 1, upper - 1,
                             2**63 - 1, Side.Buy)
        self.assertEqual((event.order_id, event.timestamp_ns, event.qty), (upper - 1,) * 3)

        with TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.bin"
            path.write_bytes((FIXTURES / "session.bin").read_bytes()[:8 + 36 + 5])
            engine = MatchingEngine()
            report = engine.replay(str(path), ReplayFormat.Binary)
            self.assertEqual(report.events, 1)
            self.assertEqual(report.diagnostic.error, ReplayError.Truncated)
            self.assertEqual(report.diagnostic.location, 2)
            self.assertEqual(engine.active_orders, 1)

    def test_native_numpy_snapshot_is_shared_immutable_and_owned(self):
        engine = MatchingEngine()
        engine.submit(Event.create(MsgType.New, 1, 2, 5, 99, Side.Buy))
        snapshot = engine.observation(2, -2, -5.5)
        first, second = snapshot.array(), snapshot.array()
        np.testing.assert_array_equal(first, [99, 5, 0, 0, 0, 0, 0, 0, 0, 1, -2, -5.5])
        self.assertEqual(first.dtype, np.dtype(np.float32))
        self.assertEqual(first.shape, (12,))
        self.assertTrue(first.flags.c_contiguous)
        self.assertFalse(first.flags.owndata)
        self.assertFalse(first.flags.writeable)
        self.assertTrue(np.shares_memory(first, second))
        self.assertEqual(first.ctypes.data, second.ctypes.data)
        with self.assertRaises(ValueError):
            first.setflags(write=True)
        with self.assertRaises(ValueError):
            second[0] = 0
        engine.submit(Event.create(MsgType.Cancel, 1, 3, 5))
        third = engine.observation(2).array()
        self.assertFalse(np.shares_memory(first, third))
        np.testing.assert_array_equal(third, np.zeros(12))
        del snapshot, engine, second
        gc.collect()
        self.assertEqual(first[0], 99)


class EnvironmentTests(unittest.TestCase):
    def test_seeded_policy_episode_fill_cancel_completion_and_replay(self):
        envs = [BookEnv(depth=3, scenario_steps=4) for _ in range(2)]
        obs, info = envs[0].reset(seed=0)
        other, other_info = envs[1].reset(seed=0)
        np.testing.assert_array_equal(obs, other)
        self.assertEqual(info, other_info)
        self.assertEqual(info["queue"], [])
        initial = obs.copy()

        for step, action in enumerate((Action(ActionKind.QUOTE_BUY, 8.9, 0.4), 0, 5, 0), 1):
            output, control = [env.step(action) for env in envs]
            np.testing.assert_array_equal(output[0], control[0])
            self.assertEqual(output[1:], control[1:])
            snapshot, reward, terminated, truncated, details = output
            self.assertEqual(details["step"], step)
            self.assertEqual(terminated, step == 4)
            self.assertFalse(truncated)
            if step == 1:
                order_id, position = details["queue"][0]
                self.assertEqual(position.order.remaining, 8)
                self.assertEqual(position.lots_ahead, 0)
            elif step == 2:
                self.assertEqual(details["fills"][0].maker_id, order_id)
                self.assertEqual(details["inventory"], 2)
                self.assertEqual(details["cash"], -200)
                self.assertEqual(details["unrealized_pnl"], 1)
                self.assertEqual(reward, 1)
                self.assertEqual(snapshot[-2:].tolist(), [2, 1])
            elif step == 3:
                self.assertEqual(details["action_result"].canceled, 6)
                self.assertEqual(details["queue"], [])
                self.assertEqual(reward, -1)
        with self.assertRaises(RuntimeError):
            envs[0].step(0)

        engine, report = envs[0].replay(FIXTURES / "session.bin", "binary")
        self.assertIsNone(report.diagnostic)
        self.assertEqual(report.events, 12)
        self.assertEqual(engine.active_orders, 2)
        with self.assertRaises(RuntimeError):
            envs[0].step(0)
        envs[0].reset(seed=4)
        del envs
        gc.collect()
        np.testing.assert_array_equal(obs, initial)

    def test_invalid_actions_preserve_state_and_cancel_indices_compact(self):
        env = BookEnv(scenario="quiet", scenario_steps=20, max_steps=4)
        env.reset(seed=20)
        for invalid in (-1, 5, 1.5, True, Action(ActionKind.QUOTE_BUY, 0.9),
                        Action(ActionKind.QUOTE_SELL, float("inf")),
                        Action(ActionKind.MARKET_BUY, 1, 4),
                        Action(ActionKind.QUOTE_BUY, 1, 100),
                        Action(ActionKind.HOLD, cancel_index=0)):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                env.step(invalid)
        *_, info = env.step(1)
        self.assertEqual(info["step"], 1)
        first = info["queue"][0][0]
        *_, info = env.step(1)
        second, position = info["queue"][1]
        self.assertEqual(position.lots_ahead, 4)
        *_, info = env.step(Action(ActionKind.CANCEL, cancel_index=0))
        self.assertNotEqual(first, second)
        self.assertEqual(info["queue"][0][0], second)
        self.assertEqual(info["queue"][0][1].orders_ahead, 0)
        _, _, terminated, truncated, info = env.step(5)
        self.assertFalse(terminated)
        self.assertTrue(truncated)
        self.assertEqual(info["queue"], [])

    def test_average_cost_and_self_trade(self):
        env = BookEnv(scenario="quiet")
        env.reset(seed=0)
        *_, info = env.step(Action(ActionKind.MARKET_BUY, 2))
        self.assertEqual(info["inventory"], 2)
        self.assertEqual(info["unrealized_pnl"], -2)
        *_, info = env.step(Action(ActionKind.MARKET_SELL, 3))
        self.assertEqual(info["inventory"], -1)
        self.assertEqual(info["cash"], 95)
        self.assertEqual(info["realized_pnl"], -4)
        self.assertEqual(info["unrealized_pnl"], -1)
        *_, info = env.step(1)
        self.assertEqual(info["inventory"], -1)
        *_, info = env.step(Action(ActionKind.MARKET_SELL, 1))
        self.assertEqual(info["fills"][0].qty, 1)
        self.assertEqual(info["cash"], 95)
        self.assertEqual(info["inventory"], -1)


if __name__ == "__main__":
    unittest.main()
