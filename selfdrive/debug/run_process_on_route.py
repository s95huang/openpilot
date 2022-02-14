#!/usr/bin/env python3

import argparse

from selfdrive.test.process_replay.compare_logs import save_log
from selfdrive.test.process_replay.process_replay import CONFIGS, replay_process
from tools.lib.logreader import MultiLogIterator
from tools.lib.route import Route

if __name__ == "__main__":
  parser = argparse.ArgumentParser(description="Run process on route and create new logs",
                                   formatter_class=argparse.ArgumentDefaultsHelpFormatter)
  parser.add_argument("route", help="The route name to use")
  parser.add_argument("process", help="The process to run")
  args = parser.parse_args()

  cfg = [c for c in CONFIGS if c.proc_name == args.process][0]

  route = Route(args.route)
  lr = MultiLogIterator(route.log_paths())
  inputs = list(lr)

  outputs = replay_process(cfg, inputs)

  # Remove message generated by the process under test and merge in the new messages
  produces = {o.which() for o in outputs}
  inputs = [i for i in inputs if i.which() not in produces]
  outputs = sorted(inputs + outputs, key=lambda x: x.logMonoTime)

  fn = f"{args.route}_{args.process}.bz2"
  save_log(fn, outputs)
