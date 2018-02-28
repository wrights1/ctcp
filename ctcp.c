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
#include "ctcp_sys.h"
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

    conn_t *conn;                         /* Connection object -- needed in order to figure
                                             out destination when sending */
    linked_list_t *segments;    /* Linked list of segments sent to this connection.
                                   It may be useful to have multiple linked lists
                                   for unacknowledged segments, segments that
                                   haven't been sent, etc. */
    uint32_t seqno;
    uint32_t ackno;

    ctcp_config_t *cfg;

    ctcp_segment_t *sent;

    uint64_t timeSent;

    uint16_t retransCount;

    uint8_t finSent;
    uint8_t finSentAcked;
    uint8_t finRecv;

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

// int compute_segLength(ctcp_segment_t *segment)
// {
//     int dataLen = 0;
//
//     if (sizeof(segment->data))
//     {
//         dataLen = strlen(segment->data);
//     }
//
//     return sizeof(ctcp_segment_t) + strlen(dataLen * sizeof(char));
// }

/*
 * Send the given segment over the connection in the given state struct.
 */
int ctcp_send(ctcp_state_t *state, ctcp_segment_t *segment)
{
    state->sent = segment;
    state->timeSent = current_time();
    printf("time = %li\n", state->timeSent);

    int dataLen = 0;
    if (segment->data != NULL) {
        dataLen = strlen(segment->data);
    }

    int segLength = sizeof(ctcp_segment_t) + (dataLen * sizeof(char));
    int sentBytes = conn_send(state->conn, segment, ntohs(segment->len));

    #if DEBUG
    printf("sentBytes = %d, segLength = %d\n", sentBytes, segLength);

    printf("--- send\n");
    print_hdr_ctcp(segment);
    printf("--- send end\n\n");
    #endif

    return sentBytes;
}

/*
 * Make a segment based on the current connection with the given data and flags
 */
ctcp_segment_t *make_segment(ctcp_state_t *state, char *buf, uint32_t flags)
{
    int dataLen = 0;
    if (buf != NULL) {
        dataLen = strlen(buf);
    }

    ctcp_segment_t *segment;
    segment = malloc(sizeof(ctcp_segment_t) + (dataLen * sizeof(char)));

    if (buf != NULL) {
        strcpy(segment->data, buf);

        #if DEBUG
        //printf("input length = %i\n",int(strlen(buf)));
        printf("buf = %s\n", segment->data);
        #endif
    }

    // initialize fields in the cTCP header
    int segLength = sizeof(ctcp_segment_t) + (dataLen * sizeof(char));
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
 */
int verify_cksum(ctcp_segment_t *segment)
{
    uint16_t original_cksum = segment->cksum;

    segment->cksum = 0;
    uint16_t computed_cksum = cksum(segment, ntohs(segment->len));
    segment->cksum = original_cksum;

    //printf("computed_cksum = %x, original_cksum = %x\n", computed_cksum,
    //       original_cksum);

    return original_cksum == computed_cksum;
}

/*
 * Convert fields in the given segment to host byte order.
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
 * Convert fields in the given segment to network byte order
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

    /* FIXME: Do any other cleanup here. */

    free(state->cfg);

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
        // allocate the input buffer
        char *buf;
        buf = calloc(sizeof(char), MAX_SEG_DATA_SIZE - sizeof(ctcp_segment_t));
        size_t len = MAX_SEG_DATA_SIZE - sizeof(ctcp_segment_t);

        // read the input
        int ret = 0;
        ret = conn_input(state->conn, buf, len);
        printf("ret = %d\n", ret);
        printf("strlen(buf) = %lu\n", strlen(buf));

        if (ret == -1) // EOF found
        {
            // send our FIN
            ctcp_segment_t *finSeg = make_segment(state, NULL, FIN | ACK);
            ctcp_send(state, finSeg);
            state->finSent = 1;
        } else if (ret > 0) {
            ctcp_segment_t *segment = make_segment(state, buf, ACK);
            ctcp_send(state, segment);

            if (!verify_cksum(segment)) {
                printf("wtf??\n");
            }
        }

        free(buf);
    }
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len)
{
    #if DEBUG
    printf("\n--- recv \n");
    print_hdr_ctcp(segment);
    #endif

    // use the segment's checksum to verify the packet.
    if (!verify_cksum(segment)) {
        #if DEBUG
        printf("corrupted segment\n");
        printf("---\n");
        #endif

        free(segment);
        return;
    }

    convert_to_host_order(segment);

    // if we receive an ACK and we sent a FIN.
    if ((segment->flags & ACK) && state->finSent == 1) {
        state->finSentAcked = 1;

        state->seqno = segment->ackno;
        state_list->timeSent = 0;
        free(state->sent);
        state->sent = NULL;
        state->retransCount = 0;

        if (state->finRecv) {
            #if DEBUG
            printf("\n--- recv end\n\n");
            #endif

            ctcp_destroy(state);
        }
    }

    if ((segment->flags & FIN)) {
        state->finRecv = 1;
        state->ackno += 1;

        ctcp_segment_t *ackSeg = make_segment(state, NULL, ACK);
        ctcp_send(state, ackSeg);

        if (state->finSentAcked) {
            #if DEBUG
            printf("\n--- recv end\n\n");
            #endif

            ctcp_destroy(state);
        }
    }


    // If the ACK flag is turned on, update our seq number.
    if (segment->flags & ACK) {
        state->seqno = segment->ackno;
        state_list->timeSent = 0;
        free(state->sent);
        state->sent = NULL;
        state->retransCount = 0;

        #if DEBUG
        printf("received ackno = %u\n", segment->ackno);
        #endif
    }

    size_t received_data_len = len - sizeof(ctcp_segment_t);
    if (received_data_len > 0) {
        conn_output(state->conn, segment->data, strlen(segment->data));

        // update our ack number
        state->ackno += received_data_len;

        #if DEBUG
        printf("received len = %lu\n", received_data_len);
        #endif

        // construct an ACK segment - only if we receive data.
        ctcp_segment_t *ack_segment = make_segment(state, NULL, ACK);
        print_hdr_ctcp(ack_segment);
        conn_send(state->conn, ack_segment, sizeof(ctcp_segment_t));

        // free the ACK segment
        free(ack_segment);
    }

    #if DEBUG
    printf("--- recv end\n\n");
    #endif

    // free resources
    free(segment);
}

void ctcp_output(ctcp_state_t *state)
{
    printf("we outputting?\n");
}

void ctcp_timer()
{
    ctcp_state_t *state = state_list;

    while (state != NULL) {
        if (state->timeSent == 0) {
            state = state->next;
            continue;
        } // haven't sent anything yet

        if (state->retransCount >= 5) {
            ctcp_destroy(state);
        }

        if ((current_time() - state->timeSent) > state->cfg->rt_timeout) {
            printf("timed out\n");
            ctcp_segment_t *segment = state->sent;
            ctcp_send(state, segment);

            state->retransCount += 1;
        }

        state = state->next;
    }
}
