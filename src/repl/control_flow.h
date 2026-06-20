/*
 * src/repl/control_flow.h - Shared REPL control-flow execution limits.
 */
#ifndef REPL_CONTROL_FLOW_H
#define REPL_CONTROL_FLOW_H

/* Hard cap on goto jumps while walking one flat program, shared by the
 * executor and replay-annotation walker. A program that exceeds this is
 * treated as a runaway goto loop and bailed out with a status message. */
#define REPL_GOTO_LOOP_LIMIT 100000

#endif /* REPL_CONTROL_FLOW_H */
