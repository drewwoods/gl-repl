#ifndef REPL_REPLAY_ANNOTATIONS_H
#define REPL_REPLAY_ANNOTATIONS_H

void repl_replay_annotations_prepare(void);

int  repl_replay_annotation_flat_cmd_for_source(int src_line);
int  repl_replay_annotation_extra_rows_for_line(int cmd_idx);

int  repl_replay_build_subst_annotation(int cmd_idx, int flat_idx,
                                        char *subst, int subst_size,
                                        char *var_comment, int comment_size);
int  repl_replay_build_eval_annotation(int cmd_idx, int flat_idx,
                                       char *eval_buf, int eval_size);

int  repl_replay_code_panel_get_command_display_text(int cmd_idx, char *out, int out_size);

#endif /* REPL_REPLAY_ANNOTATIONS_H */
