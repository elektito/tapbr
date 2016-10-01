#!/usr/bin/env python3

import argparse
from pydbus import SystemBus

bus = obj = None

def get_stats(args):
    global obj
    stats = obj.GetStats()
    print('tapbr stats:')
    for name, value in stats.items():
        print('   {}: {}'.format(name, value))

def main():
    parser = argparse.ArgumentParser(
        description='Query or send control commands to tapbr.')

    subparsers = parser.add_subparsers()

    get_stats_parser = subparsers.add_parser(
        'get-stats',
        help='Get current stats.')
    get_stats_parser.set_defaults(func=get_stats)

    args = parser.parse_args()

    if args == argparse.Namespace():
        print('No commands specified.')
        return

    global bus, obj
    try:
        bus = SystemBus()
        obj = bus.get(
            "com.elektito.tapbr", # Bus name
            "/com/elektito/tapbr" # Object path
        )

        args.func(vars(args))
    except Exception as e:
        print(e)
        return

if __name__ == '__main__':
    main()
