#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <dlfcn.h>

#include "banking.h"
#include "structures.h"
#include "ipc.h"
#include "pa2345.h"

static const char* const manual =
        "Sample of our IPC library using in bank robbery simulation\n"
        "Execution instruction: -p NUMBER_OF_PROCESSES will start simulation of balances for this processes\n"
        "ATTENTION: NUMBER OF PROCESSES SHOULD BE FROM SEGMENT [0;10]";

pid_t pid_of_processes[MAXIMAL_PROCESSES_NUMBER + 1];
balance_t initial_balances_of_clients[MAXIMAL_PROCESSES_NUMBER + 1];


void renew_metrics(int state_sum, timestamp_t time) {
    if (time <= MAX_T) {
        int current_length = message.balance_history.s_history_len;
        if (current_length == time) {
            balance_t new_balance = message.balance_history.s_history[current_length - 1].s_balance + state_sum;
            current_length++;
            message.balance_history.s_history[current_length - 1] = (BalanceState) {
                    .s_time = time,
                    .s_balance = new_balance,
                    .s_balance_pending_in = 0,
            };
            message.balance_history.s_history_len++;
        } else if (current_length - time == 1) {
            message.balance_history.s_history[current_length - 1].s_balance += state_sum;
        } else if (time - current_length > 0) {
            balance_t last_balance = message.balance_history.s_history[current_length - 1].s_balance;
            for (int index = current_length; index < time; index++) {
                message.balance_history.s_history[index] = (BalanceState) {
                        .s_time = index,
                        .s_balance = last_balance,
                        .s_balance_pending_in = 0,
                };
            }
            message.balance_history.s_history[time] = (BalanceState) {
                    .s_time = time,
                    .s_balance = last_balance + state_sum,
                    .s_balance_pending_in = 0,
            };
            message.balance_history.s_history_len = time + 1;
        }
    }
}

void pipes_cleanup() {
    for (size_t pipe_i = 0; pipe_i < MAXIMAL_PROCESSES_NUMBER; pipe_i++) {
        for (size_t pipe_j = 0; pipe_j < MAXIMAL_PROCESSES_NUMBER; pipe_j++) {
            if (pipe_i != pipe_j && pipe_i != message.message_id && pipe_j != message.message_id) {
                close(write_matrix[pipe_i][pipe_j]);
                close(read_matrix[pipe_i][pipe_j]);
            }
            if (pipe_i == message.message_id && pipe_j != message.message_id) {
                close(read_matrix[pipe_i][pipe_j]);
            }
            if (pipe_i != message.message_id && pipe_j == message.message_id ) {
                close(write_matrix[pipe_i][pipe_j]);
            }
        }
    }
}

void connect_to_children_processes(local_id parrend_process_id) {
    if (message.message_id == parrend_process_id) {
        for (size_t i = 1; i <= MAXIMAL_PROCESSES_NUMBER; i++) {
            waitpid(pid_of_processes[i], NULL, 0);
        }
    }
}


void share_handler(Message* source_message, Message* content) {
    timestamp_t current_physical_time = get_physical_time();
    Transfer* pointer_to_transfer = (Transfer*) content -> s_payload;
    TransferOrder transfer_order = pointer_to_transfer -> transfer_order;
    if (message.message_id == transfer_order.s_dst) {
        renew_metrics(transfer_order.s_amount, current_physical_time);
        //transfer_in_fmt(transfer_order.s_src, transfer_order.s_dst, transfer_order.s_amount);
        Message ack_message = {
                .s_header = {
                        .s_magic = MESSAGE_MAGIC,
                        .s_type = ACK,
                        .s_local_time = current_physical_time,
                        .s_payload_len = 0,
                },
        };
        ack_message.s_header.s_payload_len = strlen(ack_message.s_payload);
        send(&message, PARENT_ID, &ack_message);
    }
    else {
        renew_metrics(-transfer_order.s_amount, current_physical_time);
        //transfer_out_fmt(transfer_order.s_src, transfer_order.s_dst, transfer_order.s_amount);
        pointer_to_transfer -> message_id = message.message_id;
        transfer(
                pointer_to_transfer,
                transfer_order.s_src,
                transfer_order.s_dst,
                transfer_order.s_amount
        );
    }
}

int release_cs(const void* source) {
    AllHistory* history = (AllHistory*) source;
    Message release_message;
    for (size_t current_source = 1; current_source <= history->s_history_len ; current_source += 1){
        receive(&message, current_source, &release_message);
        if (release_message.s_header.s_type == BALANCE_HISTORY) {
            BalanceHistory* balance_history = (BalanceHistory*) release_message.s_payload;
            history -> s_history[balance_history -> s_id - 1] = *balance_history;
        }
        else {
            //printf("History reading process has failed.\n");
            return IPC_MESSAGE_STATUS_ERROR_CANNOT_READ;
        }
    }
    return IPC_MESSAGE_STATUS_SUCCESS;
}

int main(int argc, char * argv[]) {
    int limit_arg_number = 3;

    int has_enough_args = argc >= limit_arg_number;
    int has_flag = strcmp(argv[1], "-p") == 0;
    int strtol_is_valid = strtol(argv[2], NULL, 10) == argc - limit_arg_number;

    size_t children_process_number;
    if (has_enough_args && has_flag && strtol_is_valid) {
        children_process_number = strtol(argv[2], NULL, 10);
        if (children_process_number >= MAXIMAL_PROCESSES_NUMBER) {
            //log_errorfmt(manual);
            return 1;
        } else {
            for (int current_child = 1; current_child <= children_process_number; current_child += 1) {
                initial_balances_of_clients[current_child] = strtol(
                        argv[current_child + 2],
                        NULL,
                        10
                );
            }
        }
    } else {
        //log_errorfmt(manual);
        return 1;
    }

//    if (children_process_number >= MAXIMAL_PROCESSES_NUMBER) {
//        //log_errorfmt(manual);
//        return 1;
//    }
    number_of_processes = children_process_number + 1;
    //log_init();
    int failed_to_open_pipes_exit_code = 2;
    for (size_t i = 0; i < number_of_processes; i += 1) {
        for (size_t j = 0; j < number_of_processes; j += 1) {
            if (i == j) {
                continue;
            } else {
                int fd[2]; pipe(fd);
                if (fcntl(fd[0], FSETFL, fcntl(fd[0], F_GETFL, 0) | O_NONBLOCK) < 0
                    || fcntl(fd[1], F_SETFL, fcntl(fd[1], F_GETFL, 0) | O_NONBLOCK)) {
                    exit(failed_to_open_pipes_exit_code);
                }
                read_matrix[i][j] = fd[0];
                write_matrix[i][j] = fd[1];
                pipe_opened(i, j);
            }
        }
    }

    pid_of_processes[PARENT_ID] = getpid();

    for (size_t current_id = 1; current_id <= children_process_number; current_id += 1) {
        int child_pid = fork();
        if (child_pid == 0) {
            message = (ipc_message) {
                    .balance_history = (BalanceHistory) {
                            .s_id = current_id,
                            .s_history[0] = (BalanceState) {
                                .s_balance_pending_in = 0,
                                .s_time = 0,
                                .s_balance = initial_balances_of_clients[current_id],
                            },
                            .s_history_len = 1,
                    },
                    .message_id = current_id,
            };
            break;
        } else if (child_pid > 0) {
            message = (ipc_message) {.message_id = PARENT_ID,};
            pid_of_processes[current_id] = child_pid;
        } //else {
            //log_errorfmt("Error: Cannot create process. Parrent: %d", pid_of_processes[PARENT_ID]);
        //}
    }

    pipes_cleanup();
    ipc_message_status status;

    if (message.message_id == PARENT_ID) {
        //started();
        Message content;
        receive_from_all_children(&message, &content, children_process_number);
        //received_all_started();

        Transfer msg_transfer = {.message_id = message.message_id,};
        bank_robbery(&msg_transfer, children_process_number);

        status = send_stop_to_all(&message);
        if (status != IPC_MESSAGE_STATUS_SUCCESS) {
            //log_errorfmt("Error: failed to send multicast from %d. Code: %d\n", local.ipc_id, status);
        }
        // Receive ALL DONE
        receive_from_all_children(&message, &content, children_process_number);
        //received_all_done();


        // Receive ALL HISTORY
        AllHistory all_history = {.s_history_len = children_process_number,};
        status = release_cs(&all_history);

        if (status != IPC_MESSAGE_STATUS_SUCCESS) {
            //log_errorfmt("Error: failed to receive message. Code: %d\n", status);
        }
        else {
            print_history(&all_history);
        }
    }
    else {
        Message content;
        send_started_to_all(&message);
        //started();
        receive_from_all_children(&message, &content, children_process_number);
        //received_all_started();
        int16_t type = -1;
        local_id children_processes_left = children_process_number - 1;
        for (;;) {
            if (type == STOP) {
                break;
            } else {
                type = receive_any(&message, &content);
                if (type == TRANSFER) {
                    share_handler(&message, &content);
                } else if (type == DONE) {
                    children_processes_left--;
                } else if (type == STOP) {
                    send_done_to_all(&message);
                }
            }
        }
        type = -1;
        while(children_processes_left > 0) {
            type = receive_any(&message, &content);
            if (type == TRANSFER) {
                share_handler(&message, &content);
            } else if (type == DONE) {
                children_processes_left--;
            }
        }
        //received_all_done();
        timestamp_t current_physical_time = get_physical_time();
        renew_metrics(0, current_physical_time);
        Message msg_hisory = {
            .s_header = {
                .s_type = BALANCE_HISTORY,
                .s_local_time = current_physical_time,
                .s_magic = MESSAGE_MAGIC,
                .s_payload_len = sizeof(BalanceHistory),
            },
        };
        memcpy(
            &msg_hisory.s_payload,
            &message.balance_history,
            sizeof(BalanceHistory)
        );
        status = send(&message, PARENT_ID, &msg_hisory);
        if (status != IPC_MESSAGE_STATUS_SUCCESS) {
            //log_errorfmt("Error: failed to send multicast from %d. Code: %d\n", local.ipc_id, status);
        }
    }
    connect_to_children_processes(PARENT_ID);
    //log_end();
    return 0;
}