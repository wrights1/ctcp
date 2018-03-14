/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *     - ctcp.h: Headers for this file.
 *     - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *     - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                                 definition.
 *     - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_utils.h"

#define DEBUG 1


/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
struct ctcp_state {
    struct ctcp_state *next;    /* Next in linked list */
    struct ctcp_state **prev; /* Prev in linked list */

    conn_t *conn;               /* Connection object -- needed in order to figure
                                   out destination when sending */
    linked_list_t *segments;    /* Linked list of segments sent to this connection.
                                   It may be useful to have multiple linked lists
                                   for unacknowledged segments, segments that
                                   haven't been sent, etc. */

    uint32_t send_base;
    uint32_t current_seqno;
    uint32_t ackno;
    uint32_t next_byte_consume;

    ctcp_config_t *cfg;

    linked_list_t *sent;
    linked_list_t *received;

    uint8_t finSent;
    uint8_t finSentAcked;
    uint8_t finRecv;

    int inputSize;

    char *output_data;
    size_t received_data_len;

    uint16_t send_window_avail;
    uint16_t recv_window_avail;
    uint16_t advertised_window_size;
};

typedef struct segment_info {
    ctcp_segment_t* segment;
    int dataLen;
    long timeSent;
    int retransCount;
    int sentFlag;
} segment_info_t;

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;


//==============================================================================
// Helper functions
//==============================================================================

int ctcp_send(ctcp_state_t *state, segment_info_t *info);
ctcp_segment_t *make_segment(ctcp_state_t *state, char *buf, uint32_t flags);
segment_info_t *make_segment_info(ctcp_segment_t *segment, int dataLen);
int verify_cksum(ctcp_segment_t *segment);
void convert_to_host_order(ctcp_segment_t *segment);
void convert_to_network_order(ctcp_segment_t *segment);


/*
 * Send the given segment over the connection in the given state struct.
 *
 * Parameters:
 *      state: The state struct whose connection to sent the segment over.
 *      segment: The segment to be sent.
 *
 * Return value: Number of bytes sent.
 */
int ctcp_send(ctcp_state_t *state, segment_info_t *info)
{
    long timeSent = current_time();
    int dataLen = info->dataLen;

    int sentBytes = conn_send(state->conn, info->segment,
        ntohs(info->segment->len));
    info->timeSent = timeSent;
    info->retransCount += 1;

    #if DEBUG
    int segLength = sizeof(ctcp_segment_t) + (dataLen * sizeof(char));
    fprintf(stderr, "sentBytes = %d, segLength = %d\n", sentBytes, segLength);

    fprintf(stderr, "--- send\n");
    print_hdr_ctcp(info->segment);
    fprintf(stderr, "--- send end\n\n");
    #endif

    return sentBytes;
}

/*
 * Make a segment based on the current connection with the given data and flags.
 *
 * Parameters:
 *      state: The state associated with the segment to be created.
 *      buf: Data to be contained in the segment.
 *      flags: The flags for the segment.
 *
 * Return value: A pointer to the newly created segment, which must be freed
 *               by the caller.
 */
ctcp_segment_t *make_segment(ctcp_state_t *state, char *buf, uint32_t flags)
{
    // get the length
    int dataLen = 0;
    if (buf != NULL) {
        dataLen = state->inputSize;
    }

    size_t segLength;
    if (buf != NULL) {
        segLength = sizeof(ctcp_segment_t) + dataLen * sizeof(char);
    } else {
        segLength = sizeof(ctcp_segment_t);
    }

    ctcp_segment_t *segment;
    segment = malloc(segLength);

    // copy the data into the segment, if any
    if (buf != NULL) {
        memcpy(segment->data, buf, dataLen);
    }

    // initialize fields in the cTCP header
    segment->seqno = state->current_seqno;
    segment->ackno = state->ackno;
    segment->len = segLength;

    segment->flags = 0;
    segment->flags |= flags;

    segment->window = state->recv_window_avail;
    segment->cksum = 0;

    // convert everything to network byte order
    convert_to_network_order(segment);

    // compute the checksum
    segment->cksum = cksum(segment, segLength);

    return segment;
}

segment_info_t *make_segment_info(ctcp_segment_t *segment, int dataLen)
{
    segment_info_t *info = calloc(sizeof(segment_info_t), 1);

    info->segment = segment;
    info->timeSent = 0;
    info->retransCount = 0;
    info->dataLen = dataLen;
    info->sentFlag = 0;

    return info;
}

/*
 * Verify the given segment's checksum.
 *
 * Parameter:
 *      segment: The segment to be verified.
 *
 * Return value: 1 if the checksum is valid, 0 otherwise.
 */
int verify_cksum(ctcp_segment_t *segment)
{
    // save the original checksum
    uint16_t original_cksum = segment->cksum;

    // set cksum to 0 and compute the checksum
    segment->cksum = 0;
    uint16_t computed_cksum = cksum(segment, ntohs(segment->len));
    segment->cksum = original_cksum;

    // compare the computed and original checksum
    return original_cksum == computed_cksum;
}

/*
 * Convert fields in the given segment to host byte order.
 *
 * Parameter:
 *      segment: The given segment whose fields to be converted.
 *
 * Return value: None.
 */
void convert_to_host_order(ctcp_segment_t *segment)
{
    segment->seqno = ntohl(segment->seqno);
    segment->ackno = ntohl(segment->ackno);
    segment->len = ntohs(segment->len);
    segment->flags = ntohl(segment->flags);
    segment->window = ntohs(segment->window);
    segment->cksum = ntohs(segment->cksum);
}

/*
 * Convert fields in the given segment to network byte order.
 *
 * Parameter:
 *      segment: The given segment whose fields to be converted.
 *
 * Return value: None.
 */
void convert_to_network_order(ctcp_segment_t *segment)
{
    segment->seqno = htonl(segment->seqno);
    segment->ackno = htonl(segment->ackno);
    segment->len = htons(segment->len);
    segment->flags = htonl(segment->flags);
    segment->window = htons(segment->window);
    segment->cksum = htons(segment->cksum);
}


//==============================================================================
// Main functions
//==============================================================================

ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg)
{
    /* Connection could not be established. */
    if (conn == NULL) {
        return NULL;
    }

    /* Established a connection. Create a new state and update the linked list
       of connection states. */
    ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
    state->next = state_list;
    state->prev = &state_list;
    if (state_list) {
        state_list->prev = &state->next;
    }
    state_list = state;

    /* Set fields. */
    state->conn = conn;
    state->current_seqno = 1;
    state->send_base = 1;
    state->ackno = 1;
    state->next_byte_consume = 1;

    state->cfg = cfg;

    state->output_data = calloc(MAX_SEG_DATA_SIZE, sizeof(char));

    state->sent = ll_create();
    state->send_window_avail = state->cfg->send_window;
    state->advertised_window_size = state->cfg->recv_window;

    state->received = ll_create();
    state->recv_window_avail = state->cfg->recv_window;


    return state;
}

void ctcp_destroy(ctcp_state_t *state)
{
    /* Update linked list. */
    if (state->next) {
        state->next->prev = state->prev;
    }

    *state->prev = state->next;
    conn_remove(state->conn);

    free(state->cfg);
    free(state->output_data);

    if (state->sent != NULL) {
        free(state->sent);
    }

    free(state);
    end_client();
}

/*
 ctcp_read

 calls conn_input to get stuff from stdin, then makes segment and calls
 conn_send to send it

*/
void ctcp_read(ctcp_state_t *state)
{
    if (state->finSent) {
        // if a FIN has been sent, we don't accept input anymore
        return;
    } else if (state->send_window_avail > 0){
        // otherwise, if no segment is waiting to be ACK'd, we read from STDIN.
        // allocate the input buffer
        char *buf;
        buf = calloc(sizeof(char), state->send_window_avail);
        size_t len = state->send_window_avail;

        // read the input
        int ret = 0;
        ret = conn_input(state->conn, buf, len);
        state->inputSize = ret;

        #if DEBUG
        fprintf(stderr, "ret = %d\n", ret);
        #endif

        if (ret == -1) // EOF found
        {
            // send EOF to output in ctcp_output
            // fprintf(stderr, "EOF\n");
            // conn_output(state->conn, NULL, 0);
            //
            // // send our FIN
            // ctcp_segment_t *finSeg = make_segment(state, NULL, FIN | ACK);
            // ctcp_send(state, finSeg, 0);
            state->finSent = 1;
        } else if (ret > 0) {
            // otherwise we send the inputted data
            int start = 0;
            int total_data_size = ret;

            while (start < total_data_size && state->send_window_avail > 0)
            {
                int newbuf_size = MAX_SEG_DATA_SIZE;
                if (ret < MAX_SEG_DATA_SIZE) {
                    newbuf_size = ret;
                }

                state->inputSize = (newbuf_size);

                char *newBuf = calloc(newbuf_size, 1);
                memcpy(newBuf, buf + start, newbuf_size);

                ctcp_segment_t *segment = make_segment(state, newBuf, ACK);
                segment_info_t *info = make_segment_info(segment, newbuf_size);

                ll_add(state->sent, info);

                ret = ret - newbuf_size;
                start += newbuf_size;
                state->send_window_avail -= newbuf_size;

                free(newBuf);
            }

            ll_node_t *current_node = ll_front(state->sent);
            // find first unsent segment in sent list, start sending from there
            while (current_node != NULL) {
                segment_info_t *info = (segment_info_t *) current_node->object;
                if (info->sentFlag == 0)
                    break;
                current_node = current_node->next;
            }

            segment_info_t *current_info = NULL;
            if (current_node != NULL) {
                current_info = (segment_info_t *) current_node->object;
            }

            while (current_node != NULL && state->advertised_window_size > 0 && current_info->sentFlag == 0 ) {
                //fprintf(stderr, "=============== state->current_seqno ======== %d\n", state->current_seqno);
                current_info->segment->seqno = htonl(state->current_seqno);
                state->current_seqno += current_info->dataLen;
                current_info->segment->cksum = 0;
                current_info->segment->cksum = cksum(current_info->segment, ntohs(current_info->segment->len));
                ctcp_send(state, current_info);
                //fprintf(stderr, "=============== current_info->dataLen ======== %d\n", current_info->dataLen);

                current_info->sentFlag = 1;

                current_node = current_node->next;
                if (current_node != NULL) {
                    current_info = (segment_info_t *) current_node->object;
                }

            }
        }

    free(buf);

    } else {
        //fprintf(stderr, "state->send_window_avail = 0\n");
    }
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len)
{
    #if DEBUG
    fprintf(stderr, "\n--- recv \n");
    print_hdr_ctcp(segment);
    #endif

    // use the segment's checksum to verify the packet.
    if (!verify_cksum(segment)) {
        #if DEBUG
        fprintf(stderr, "corrupted segment\n");
        fprintf(stderr, "---\n");
        #endif

        free(segment);
        return;
    }

    // convert fields to host byte order
    convert_to_host_order(segment);

    // ----------------- shutdown process --------------------------------------
    // if we receive an ACK and we sent a FIN.
    // if ((segment->flags & ACK) && state->finSent == 1) {
    //     // ensure the ACK we just got is ACKing the FIN
    //     if (segment->ackno == state->send_base + 1)
    //     {
    //         // mark our FIN as having been ACK'd and update the sequence number
    //         state->finSentAcked = 1;
    //         state->send_base = segment->ackno;
    //
    //         // free the sent segment and reset the retransmission counter.
    //         state_list->timeSent = 0;
    //         free(state->sent);
    //         state->sent = NULL;
    //         state->retransCount = 0;
    //
    //         // if we already received a FIN before sending one
    //         // and having it ACKd, teardown
    //         if (state->finRecv) {
    //             #if DEBUG
    //             fprintf(stderr, "\n--- recv end\n\n");
    //             #endif
    //
    //             free(segment);
    //             ctcp_destroy(state);
    //
    //             // exit the function in case the program's running as the server
    //             // in which case ctcp_destroy will not exit the program
    //             return;
    //         }
    //     }
    // }

    // if we receive a FIN
    // if ((segment->flags & FIN)) {
    //     if (state->finRecv == 0) {
    //         // increase the ackno if we haven't yet received a FIN.
    //         state->ackno += 1;
    //     }
    //     state->finRecv = 1;
    //
    //     // ACK the received FIN.
    //     ctcp_segment_t *ackSeg = make_segment(state, NULL, ACK);
    //     conn_send(state->conn, ackSeg, sizeof(ctcp_segment_t));
    //
    //     #if DEBUG
    //     print_hdr_ctcp(ackSeg);
    //     #endif
    //
    //     free(ackSeg);
    //
    //     // if we receive a FIN and have already sent a FIN and had it ACKd
    //     // then exit the client.
    //     if (state->finSentAcked) {
    //         #if DEBUG
    //         fprintf(stderr, "\n--- recv end\n\n");
    //         #endif
    //
    //         free(segment);
    //         ctcp_destroy(state);
    //
    //         // exit the function in case the program's running as the server,
    //         // in which case ctcp_destroy will not exit the program
    //         return;
    //     }
    // }
    // -------------------- END shutdown process -------------------------------


    // If the ACK flag is turned on
    if (segment->flags & ACK && ll_length(state->sent) > 0) {
        //int dataLen = state->inputSize;
        state->advertised_window_size = segment->window;

        // #if DEBUG
        // fprintf(stderr, "datalen = %d, state->send_base = %d\n", dataLen,
        //     state->send_base);
        // #endif

        ll_node_t *current_node = state->sent->head;
        segment_info_t *current_info = (segment_info_t *) current_node->object;

        // update our sequence number if the segment is ACK'ing our previously
        // sent segment.
        //if (current_info->dataLen + state->send_base == segment->ackno) {
        if (state->send_base < segment->ackno) {
            state->send_base = segment->ackno;

            ll_node_t *next_node = NULL;

            #if DEBUG
            if (current_node == NULL) fprintf(stderr, "current_node == NULL\n");
            fprintf(stderr, "seqno = %u\n", ntohl(current_info->segment->seqno));
            fprintf(stderr, "send_base = %d\n", state->send_base);
            #endif

            while (current_node != NULL && ntohl(current_info->segment->seqno) < state->send_base) {
                next_node = current_node->next;
                state->send_window_avail += current_info->dataLen;

                #if DEBUG
                fprintf(stderr, "-\n");
                fprintf(stderr, "state->send_window_avail = %d\n", state->send_window_avail);
                fprintf(stderr, "current seqno = %u\n", ntohl(current_info->segment->seqno));
                fprintf(stderr, "-\n");
                #endif

                free(current_info->segment);
                free(current_info);
                ll_remove(state->sent, current_node);

                current_node = next_node;
                if (current_node != NULL) {
                    current_info = (segment_info_t *) current_node->object;
                }
            }
        } else if (state->send_base == segment->ackno) {
            fprintf(stderr, "WE LOST A BOI FAM\n");
            // there is a gap, wait for timeout

        }

    }

    // get the data size and the available space for outputting
    size_t received_data_len = segment->len - sizeof(ctcp_segment_t);
    size_t available_space = conn_bufspace(state->conn);

    fprintf(stderr, "received_data_len = %lu\n", received_data_len);
    fprintf(stderr, "available_space = %lu\n", available_space);
    fprintf(stderr, "state->recv_window_avail = %d\n", state->recv_window_avail);

    if (received_data_len == 0) {
        free(segment);
        return;
    }

    // only ACK the segment and output if there is enough space, and if
    // there is enough data
    if (/*available_space >= received_data_len && */state->recv_window_avail >= received_data_len) {
        // update and output the data if the segment isn't duplicate.

        if (segment->seqno == state->ackno) {
            ll_add_front(state->received, segment);

            ll_node_t *current_node = ll_front(state->received);
            ctcp_segment_t *current_segment = NULL;

            if (current_node != NULL) {
                current_segment = (ctcp_segment_t *) current_node->object;
            }

            while (current_node != NULL && current_segment->seqno == state->ackno) {
                int dataLen = current_segment->len - sizeof(ctcp_segment_t);
                state->ackno += dataLen;

                current_node = current_node->next;
                if (current_node != NULL) {
                    current_segment = (ctcp_segment_t *) current_node->object;
                }
            }

            state->recv_window_avail -= received_data_len;
            ctcp_output(state);

            #if DEBUG
            fprintf(stderr, "received len = %lu\n", received_data_len);
            #endif
        } else if (segment->seqno > state->ackno) {
            ll_node_t *current_node = ll_front(state->received);
            ll_node_t *prev_node = NULL;

            if (current_node == NULL) {
                ll_add_front(state->received, segment);
            } else {
                ctcp_segment_t *current_segment = (ctcp_segment_t *) current_node->object;
                int duplicate_segment = 0;

                while (current_node != NULL && current_segment->seqno <= segment->seqno) {
                    if (current_segment->seqno == segment->seqno) {
                        duplicate_segment = 1;
                        break;
                    }

                    prev_node = current_node;
                    current_node = current_node->next;

                    if (current_node != NULL) {
                        current_segment = (ctcp_segment_t *) current_node->object;
                    }
                }

                if (!duplicate_segment) {
                    if (prev_node == NULL) {
                        ll_add_front(state->received, segment);
                    } else if (current_node == NULL) {
                        ll_add(state->received, segment);
                    } else {
                        ll_add_after(state->received, prev_node, segment);
                    }
                }
            }
        }
    }

    // construct and send an ACK segment
    ctcp_segment_t *ack_segment = make_segment(state, NULL, ACK);
    conn_send(state->conn, ack_segment, sizeof(ctcp_segment_t));

    #if DEBUG
    print_hdr_ctcp(ack_segment);
    #endif

    // free the ACK segment
    free(ack_segment);

    #if DEBUG
    fprintf(stderr, "--- recv end\n\n");
    #endif

    // free received segment
    //free(segment);
}

void ctcp_output(ctcp_state_t *state)
{
    ll_node_t *current_node = ll_front(state->received);
    ctcp_segment_t *current_segment = NULL;
    if (current_node != NULL) {
        current_segment = (ctcp_segment_t *) current_node->object;
    }

    if (current_segment->seqno != state->next_byte_consume) {
        return;
    }

    while (current_node != NULL && current_segment->seqno == state->next_byte_consume) {

        size_t space = conn_bufspace(state->conn);
        int dataLen = current_segment->len - sizeof(ctcp_segment_t);

        if (space >= dataLen) {
            conn_output(state->conn, current_segment->data, dataLen);
            state->next_byte_consume += dataLen;
            state->recv_window_avail += dataLen;

            ll_node_t *next_node = current_node->next;
            free(current_segment);
            ll_remove(state->received, current_node);

            current_node = next_node;
            if (current_node != NULL) {
                current_segment = (ctcp_segment_t *) current_node->object;
            }
        } else {
            break;
        }
    }
}

void ctcp_timer()
{
    ctcp_state_t *state = state_list;

    // iterate through the list of states
    while (state != NULL) {
        ll_node_t *current_node = ll_front(state->sent);
        segment_info_t *current_info = NULL;
        if (current_node != NULL) {
            current_info = (segment_info_t *) current_node->object;
        }

        while (current_node != NULL && current_info->sentFlag == 1) {
            if (current_info->retransCount > 5) {
                ctcp_destroy(state);
            } else if ((current_time() - current_info->timeSent) > state->cfg->rt_timeout) {
                #if DEBUG
                fprintf(stderr, "timed out\n");
                #endif

                // retransmit the segment and increase the retransmission counter.
                ctcp_send(state, current_info);
            }

            current_node = current_node->next;
            if (current_node != NULL) {
                current_info = (segment_info_t *) current_node->object;
            }
        }

        state = state->next;
    }
}
