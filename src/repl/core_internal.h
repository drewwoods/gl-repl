/*
 * src/repl/core_internal.h - Shared internal parse / normalize helpers.
 *
 * The narrow internal surface that several REPL TUs and tests share:
 *
 *   - the parse-and-normalize entry points from normalize.h,
 *   - the parse / extract / canonical-text helpers from text_helpers.h, and
 *   - visible-variable collection from visible_vars.h used by parse callers.
 *
 * It owns none of scene loading, export, editor-input shims, or controller
 * hooks; those live behind their own headers (repl/scenes.h, repl/export.h,
 * src/editor/input.h, ...) which callers include directly when they need them.
 */
#ifndef REPL_CORE_INTERNAL_H
#define REPL_CORE_INTERNAL_H

#include "repl/command.h"      /* GLCmd */
#include "repl/eval.h"         /* ExprVar */
#include "repl/normalize.h"    /* parse-and-normalize entry points */
#include "repl/text_helpers.h" /* parse / extract / canonical-text helpers */
#include "repl/visible_vars.h" /* visible-variable collection */
#include "source_document.h"   /* SourceTextView document view */

#endif /* REPL_CORE_INTERNAL_H */
