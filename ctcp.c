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

#define DEBUG 0


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

    uint32_t seqno;
    uint32_t ackno;

    ctcp_config_t *cfg;

    ctcp_segment_t *sent;

    long timeSent;
    uint8_t retransCount;

    uint8_t finSent;
    uint8_t finSentAcked;
    uint8_t finRecv;

    int inputSize;

    char *output_data;
    size_t received_data_len;
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;


//==============================================================================
// Helper functions
//==============================================================================

int ctcp_send(ctcp_state_t *state, ctcp_segment_t *segment);
ctcp_segment_t *make_segment(ctcp_state_t *state, char *buf, uint32_t flags);
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
int ctcp_send(ctcp_state_t *state, ctcp_segment_t *segment)
{
    state->sent = segment;
    state->timeSent = current_time();

    int sentBytes = conn_send(state->conn, segment, ntohs(segment->len));

    #if DEBUG
    int dataLen = 0;
    if (segment->data != NULL) {
        dataLen = strlen(segment->data);
    }
    int segLength = sizeof(ctcp_segment_t) + (dataLen * sizeof(char));
    fprintf(stderr, "sentBytes = %d, segLength = %d\n", sentBytes, segLength);

    fprintf(stderr, "--- send\n");
    print_hdr_ctcp(segment);
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
    segment->seqno = state->seqno;
    segment->ackno = state->ackno;
    segment->len = segLength;

    segment->flags = 0;
    segment->flags |= flags;

    segment->window = MAX_SEG_DATA_SIZE;
    segment->cksum = 0;

    // convert everything to network byte order
    convert_to_network_order(segment);

    // compute the checksum
    segment->cksum = cksum(segment, segLength);

    return segment;
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
    state->seqno = 1;
    state->ackno = 1;

    state->sent = NULL;
    state->timeSent = 0;

    state->cfg = cfg;

    state->output_data = calloc(MAX_SEG_DATA_SIZE, sizeof(char));

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
    } else if (state->sent == NULL) {
        // otherwise, if no segment is waiting to be ACK'd, we read from STDIN.
        // allocate the input buffer
        char *buf;
        buf = calloc(sizeof(char), MAX_SEG_DATA_SIZE);
        size_t len = MAX_SEG_DATA_SIZE;

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
            fprintf(stderr, "EOF\n");
            conn_output(state->conn, NULL, 0);

            // send our FIN
            ctcp_segment_t *finSeg = make_segment(state, NULL, FIN | ACK);
            ctcp_send(state, finSeg);
            state->finSent = 1;
        } else if (ret > 0) {
            // otherwise we send the inputted data
            ctcp_segment_t *segment = make_segment(state, buf, ACK);
            ctcp_send(state, segment);
        }

        free(buf);
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
    if ((segment->flags & ACK) && state->finSent == 1) {
        // ensure the ACK we just got is ACKing the FIN
        if (segment->ackno == state->seqno + 1) 
        {
            // mark our FIN as having been ACK'd and update the sequence number
            state->finSentAcked = 1;
            state->seqno = segment->ackno;

            // free the sent segment and reset the retransmission counter.
            state_list->timeSent = 0;
            free(state->sent);
            state->sent = NULL;
            state->retransCount = 0;

            // if we already received a FIN before sending one 
            // and having it ACKd, teardown
            if (state->finRecv) {
                #if DEBUG
                fprintf(stderr, "\n--- recv end\n\n");
                #endif

                free(segment);
                ctcp_destroy(state);

                // exit the function in case the program's running as the server
                // in which case ctcp_destroy will not exit the program
                return;
            }
        }
    }

    // if we receive a FIN
    if ((segment->flags & FIN)) {
        if (state->finRecv == 0) {
            // increase the ackno if we haven't yet received a FIN.
            state->ackno += 1;
        }
        state->finRecv = 1;

        // ACK the received FIN.
        ctcp_segment_t *ackSeg = make_segment(state, NULL, ACK);
        conn_send(state->conn, ackSeg, sizeof(ctcp_segment_t));

        #if DEBUG
        print_hdr_ctcp(ackSeg);
        #endif

        free(ackSeg);

        // if we receive a FIN and have already sent a FIN and had it ACKd
        // then exit the client.
        if (state->finSentAcked) {
            #if DEBUG
            fprintf(stderr, "\n--- recv end\n\n");
            #endif

            free(segment);
            ctcp_destroy(state);

            // exit the function in case the program's running as the server,
            // in which case ctcp_destroy will not exit the program
            return;
        }
    }
    // -------------------- END shutdown process -------------------------------


    // If the ACK flag is turned on
    if (segment->flags & ACK) {
        int dataLen = state->inputSize;

        #if DEBUG
        fprintf(stderr, "datalen = %d, state->seqno = %d\n", dataLen, 
            state->seqno);
        #endif

        // update our sequence number if the segment is ACK'ing our previously
        // sent segment.
        if (state->seqno + dataLen == segment->ackno) {
            state->seqno = segment->ackno;

            free(state->sent);
            state_list->timeSent = 0;
            state->sent = NULL;
            state->retransCount = 0;

            #if DEBUG
            fprintf(stderr, "received ackno = %u\n", segment->ackno);
            #endif
        }
    }

    // get the data size and the available space for outputting
    size_t received_data_len = segment->len - sizeof(ctcp_segment_t);
    size_t available_space = conn_bufspace(state->conn);

    // only ACK the segment and output if there is enough space, and if 
    // there is enough data
    if (received_data_len > 0 && available_space >= received_data_len) {
        // update and output the data if the segment isn't duplicate.
        if (segment->seqno >= state->ackno) {
            memcpy(state->output_data, segment->data, received_data_len);
            state->received_data_len = received_data_len;
            ctcp_output(state);
            state->ackno += received_data_len;

            #if DEBUG
            fprintf(stderr, "received len = %lu\n", received_data_len);
            #endif
        }

        // construct and send an ACK segment - only if we receive data.
        ctcp_segment_t *ack_segment = make_segment(state, NULL, ACK);
        conn_send(state->conn, ack_segment, sizeof(ctcp_segment_t));

        #if DEBUG
        print_hdr_ctcp(ack_segment);
        #endif

        // free the ACK segment
        free(ack_segment);
    }

    #if DEBUG
    fprintf(stderr, "--- recv end\n\n");
    #endif

    // free received segment
    free(segment);
}

void ctcp_output(ctcp_state_t *state)
{
    // verify if there is enough space for outputting before call conn_output
    size_t space = conn_bufspace(state->conn);
    if (space >= state->received_data_len)
    {
        conn_output(state->conn, state->output_data, state->received_data_len);
        memset(state->output_data, 0, MAX_SEG_DATA_SIZE);
    }
}

void ctcp_timer()
{
    ctcp_state_t *state = state_list;

    // iterate through the list of states
    while (state != NULL) {
        if (state->timeSent == 0) {
            state = state->next;
            continue;
        } // haven't sent anything yet

        // teardown the connection if the retransmission limit is reached.
        if (state->retransCount >= 5) {
            ctcp_destroy(state);
        } else if ((current_time() - state->timeSent) > state->cfg->rt_timeout) {
            // otherwise check if the last sent segment has timed out.
            #if DEBUG
            fprintf(stderr, "timed out\n");
            #endif

            // retransmit the segment and increase the retransmission counter.
            ctcp_segment_t *segment = state->sent;
            ctcp_send(state, segment);

            state->retransCount += 1;
        }

        state = state->next;
    }
}
