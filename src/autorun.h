#pragma once

// One-shot scripted session for automated tests (no clicking in the menu):
// <exe dir>\DmCPT_autorun.txt, one command per line, is read at startup and DELETED, then executed once the
// game reached the front end (UE3 ready, game thread known, player controller in the main-menu map).
// Commands: wait <sec> | test_calls | dump_names | dump_objects | dump_classes | dump_profile | dump_scripts |
//           dump_layout | backup <tag> | unlock <step|all> | key <vk> | screenshot <name> | slot | wait_slot <s> |
//           fixture | slots | export <slot> [notes] | import <name> <slot> | load_slot <slot> | menu 0|1 [tab] | exit
// After a startup script the session keeps watching the same file: a new script runs as soon as it appears
// (test sessions only - without a startup script nothing is watched).
namespace autorun {
void Start();  // init thread
}
