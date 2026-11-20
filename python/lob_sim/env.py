"""Queue-aware policy episodes backed by the native continuous matcher."""

from dataclasses import dataclass
from enum import IntEnum
from fractions import Fraction
from math import floor
from numbers import Real
from operator import index
from os import fsdecode
from random import Random

from ._native import Event, MatchingEngine, MsgType, ReplayFormat, Side, Status


class ActionKind(IntEnum):
    HOLD = 0
    QUOTE_BUY = 1
    QUOTE_SELL = 2
    MARKET_BUY = 3
    MARKET_SELL = 4
    CANCEL = 5


@dataclass(frozen=True)
class Action:
    kind: ActionKind
    size: float = 1
    offset: float = 0
    cancel_index: int | None = None


def _integer(name, value, low, high):
    try:
        if isinstance(value, bool):
            raise TypeError
        value = index(value)
    except TypeError as error:
        raise ValueError(f"{name} must be an integer") from error
    if not low <= value <= high:
        raise ValueError(f"{name} must be in [{low}, {high}]")
    return value


def _quantize(name, value, low, high):
    if isinstance(value, bool) or not isinstance(value, Real) or not low <= value < high + 1:
        raise ValueError(f"{name} must round down to a value in [{low}, {high}]")
    return floor(value)


class BookEnv:
    """Single-agent, single-instrument episodes with seeded background liquidity."""

    _reference = 100
    _max_size = 2**32 - 1
    _max_steps = 2**62 - 1  # Leaves room for two IDs per step plus the initial quote ladder.

    def __init__(self, *, depth=5, scenario="synthetic", scenario_steps=32, max_steps=None):
        self._depth = _integer("depth", depth, 1, 65536)
        if scenario not in ("synthetic", "quiet"):
            raise ValueError("scenario must be 'synthetic' or 'quiet'")
        self._scenario = scenario
        self._scenario_steps = _integer("scenario_steps", scenario_steps, 1, self._max_steps)
        self._step_limit = (
            self._scenario_steps if max_steps is None
            else _integer("max_steps", max_steps, 1, self._max_steps)
        )
        self._engine = None
        self._finished = False

    def reset(self, seed=None):
        """Start a fresh episode and return (observation, info)."""
        seed = 0 if seed is None else _integer("seed", seed, 0, 2**64 - 1)
        self._rng = Random(seed)
        self._engine = MatchingEngine()
        self._orders = {}
        self._next_id = 0
        self._step = 0
        self._finished = False
        self._inventory = 0
        self._cash = 0
        self._open_cost = Fraction(0)
        self._mark = Fraction(self._reference)
        self._last_trade = None

        for offset in range(1, 5):
            for side, direction in ((Side.Buy, -1), (Side.Sell, 1)):
                self._submit(MsgType.New, self._new_id(), self._rng.randrange(6, 13),
                             self._reference + direction * offset, side)
        return self._observation(), self._info(None, [])

    def step(self, action):
        """Apply an action and advance one step."""
        if self._engine is None or self._finished:
            raise RuntimeError("call reset before starting an episode")
        kind, size, offset, target = self._decode(action)
        before = self._equity
        self._step += 1
        result = None
        fills = []
        if kind == ActionKind.CANCEL:
            resting = self._engine.queue_position(target).order
            result = self._submit(MsgType.Cancel, target, resting.remaining)
            if result.status == Status.Ok:
                del self._orders[target]
        elif kind != ActionKind.HOLD:
            side = Side.Buy if kind in (ActionKind.QUOTE_BUY, ActionKind.MARKET_BUY) else Side.Sell
            quote = kind in (ActionKind.QUOTE_BUY, ActionKind.QUOTE_SELL)
            price = self._reference + (-offset if side == Side.Buy else offset) if quote else None
            order_id = self._new_id()
            result = self._submit(MsgType.New if quote else MsgType.Market, order_id,
                                  size, price, side, owned_taker=True)
            fills.extend(result.fills)
            if result.remaining:
                self._orders[order_id] = None

        if self._scenario == "synthetic" and self._step % 2 == 0:
            side = self._rng.choice((Side.Buy, Side.Sell))
            flow = self._submit(MsgType.Market, self._new_id(), self._rng.randrange(1, 4), side=side)
            fills.extend(flow.fills)
        self._update_mark()

        terminated = self._step == self._scenario_steps
        truncated = not terminated and self._step == self._step_limit
        self._finished = terminated or truncated
        return (self._observation(), float(self._equity - before), terminated, truncated,
                self._info(result, fills))

    def replay(self, path, format="csv", on_event=None):
        """Return (separate matching engine, replay report) for a normalized event file."""
        if format == "csv":
            format = ReplayFormat.Csv
        elif format == "binary":
            format = ReplayFormat.Binary
        elif not isinstance(format, ReplayFormat):
            raise ValueError("format must be 'csv', 'binary', or a ReplayFormat")
        engine = MatchingEngine()
        return engine, engine.replay(fsdecode(path), format, on_event)

    def _decode(self, action):
        if isinstance(action, Action):
            kind = ActionKind(_integer("kind", action.kind, 0, int(ActionKind.CANCEL)))
        else:
            last_code = int(ActionKind.MARKET_SELL) + len(self._orders)
            code = _integer("action", action, 0, last_code)
            if code >= ActionKind.CANCEL:
                action = Action(ActionKind.CANCEL, cancel_index=code - ActionKind.CANCEL)
            else:
                action = Action(ActionKind(code), size=4 if code else 1)
            kind = action.kind

        if kind == ActionKind.CANCEL:
            slot = _integer("cancel_index", action.cancel_index, 0, len(self._orders) - 1)
            target = list(self._orders)[slot]
        else:
            if action.cancel_index is not None:
                raise ValueError("cancel_index is valid only for a cancel action")
            target = None
        takes_size = kind not in (ActionKind.HOLD, ActionKind.CANCEL)
        takes_offset = kind in (ActionKind.QUOTE_BUY, ActionKind.QUOTE_SELL)
        if not takes_size and action.size != 1:
            raise ValueError("size is valid only for quote and market actions")
        if not takes_offset and action.offset != 0:
            raise ValueError("offset is valid only for quote actions")
        size = _quantize("size", action.size, 1, self._max_size)
        offset = _quantize("offset", action.offset, 0, self._reference - 1)
        return kind, size, offset, target

    def _new_id(self):
        order_id = self._next_id
        self._next_id += 1
        return order_id

    def _submit(self, kind, order_id, size, price=None, side=None, *, owned_taker=False):
        event = Event.create(kind, order_id, self._step, size, price, side)
        if isinstance(event, Status):
            raise RuntimeError(f"invalid internally generated event: {event}")
        result = self._engine.submit(event)
        for fill in result.fills:
            self._last_trade = fill.price_ticks
            owned_maker = fill.maker_id in self._orders
            if owned_taker != owned_maker:
                buy = (fill.taker_side == Side.Buy) == owned_taker
                self._account(fill.qty if buy else -fill.qty, fill.price_ticks)
            if owned_maker and self._engine.queue_position(fill.maker_id) is None:
                del self._orders[fill.maker_id]
        return result

    def _account(self, signed_lots, price):
        inventory = self._inventory
        self._inventory += signed_lots
        self._cash -= signed_lots * price
        if inventory * signed_lots < 0:
            closed = min(abs(inventory), abs(signed_lots))
            self._open_cost *= Fraction(abs(inventory) - closed, abs(inventory))
            signed_lots += closed if signed_lots < 0 else -closed
        self._open_cost += signed_lots * price

    def _update_mark(self):
        bids = self._engine.depth(Side.Buy, 1)
        asks = self._engine.depth(Side.Sell, 1)
        if bids and asks:
            self._mark = Fraction(bids[0].price_ticks + asks[0].price_ticks, 2)
        elif self._last_trade is not None:
            self._mark = Fraction(self._last_trade)

    @property
    def _equity(self):
        return self._cash + self._inventory * self._mark

    @property
    def _unrealized(self):
        return self._inventory * self._mark - self._open_cost

    def _observation(self):
        return self._engine.observation(self._depth, self._inventory, float(self._unrealized)).array()

    def _info(self, result, fills):
        return {
            "action_result": result,
            "fills": fills,
            "queue": [(order_id, self._engine.queue_position(order_id)) for order_id in self._orders],
            "step": self._step,
            "inventory": self._inventory,
            "cash": self._cash,
            "mark": float(self._mark),
            "realized_pnl": float(self._cash + self._open_cost),
            "unrealized_pnl": float(self._unrealized),
            "equity": float(self._equity),
        }
