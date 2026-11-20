"""Run a synthetic policy episode and optionally replay a normalized session."""

from argparse import ArgumentParser

from . import Action, ActionKind, BookEnv


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("--replay", help="path to a normalized event fixture")
    parser.add_argument("--format", choices=("csv", "binary"), default="csv")
    args = parser.parse_args()

    env = BookEnv(depth=3, scenario_steps=4)
    obs, _ = env.reset(seed=0)
    print(f"Initial observation {obs.tolist()}")
    actions = (Action(ActionKind.QUOTE_BUY, size=8), 0, 5, 0)
    for action in actions:
        _, reward, terminated, truncated, info = env.step(action)
        print(f"Step {info['step']}, fills {len(info['fills'])}, inventory {info['inventory']}, "
              f"reward {reward:g}, terminated {terminated}, truncated {truncated}")
    if args.replay:
        engine, report = env.replay(args.replay, args.format)
        if report.diagnostic:
            parser.exit(1, f"Replay location {report.diagnostic.location}: {report.diagnostic.message}\n")
        print(f"Replayed {report.events} events, {engine.active_orders} live orders")


if __name__ == "__main__":
    main()
