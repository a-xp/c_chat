#ifndef __BABBLE_COMMANDS_H__
#define __BABBLE_COMMANDS_H__

int process_command(command_t *cmd);

/* Operations */
int run_login_command(command_t *cmd);
int run_publish_command(command_t *cmd);
int run_follow_command(command_t *cmd);
int run_timeline_command(command_t *cmd);
int run_fcount_command(command_t *cmd);
int run_rdv_command(command_t *cmd);

int unregisted_client(command_t *cmd);

#endif