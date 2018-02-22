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
                                                             haven't been sent, etc. Lab 1 uses the
                                                             stop-and-wait protocol and therefore does not
                                                             necessarily need a linked list. You may remove
                                                             this if this is the case for you */

    uint32_t seqno;
    uint32_t ackno;
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
                    code! Helper functions make the code clearer and cleaner. */

#define DEBUG 1

ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
    /* Connection could not be established. */
    if (conn == NULL) {
        return NULL;
    }

    /* Established a connection. Create a new state and update the linked list
         of connection states. */
    ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
    state->next = state_list;
    state->prev = &state_list;
    if (state_list)
        state_list->prev = &state->next;
    state_list = state;

    /* Set fields. */
    state->conn = conn;
    state->seqno = 1;
    state->ackno = 1;

    return state;
}

void ctcp_destroy(ctcp_state_t *state) {
    /* Update linked list. */
    if (state->next)
        state->next->prev = state->prev;

    *state->prev = state->next;
    conn_remove(state->conn);

    /* FIXME: Do any other cleanup here. */

    free(state);
    end_client();
}
/*
 ctcp_read

 calls conn_input to get stuff from stdin, then makes segment and calls
 conn_send to send it

*/
void ctcp_read(ctcp_state_t *state) {
    // allocate the input buffer
    char *buf;
    buf = calloc(sizeof(char), MAX_SEG_DATA_SIZE);
    size_t len = 100;

    // read the input
    int ret;
    ret = conn_input(state->conn, buf, len);

    // create a cTCP segment with the input data
    ctcp_segment_t *segment = malloc(sizeof(ctcp_segment_t) + (strlen(buf) * sizeof(char)));
    strcpy(segment->data,buf);

    #if DEBUG
    printf("---\n");
    printf("input length = %i\n",ret);
    printf("buf = %s\n",segment->data);
    #endif

    // initialize fields in the cTCP header
    int segLength = sizeof(ctcp_segment_t) + (strlen(segment->data) * sizeof(char));
    segment->seqno = htonl(state->seqno);
    segment->ackno = htonl(state->ackno);
    segment->len = htons(segLength);
    segment->flags |= ACK;
    segment->window = htons(MAX_SEG_DATA_SIZE);
    segment->cksum = 0;
    segment->flags = htonl(segment->flags);

    // compute the checksum
    segment->cksum = cksum(segment,segLength);

    // send the cTCP segment

    /* TODO: what if sentBytes is less than segLength? Should we send again
     * until we have sent all the bytes, like we did with prog1 (assuming that
     * was what your team also did)?
     * 
     * Feel free to change the below too.
     */
    // int sentBytes = 0;
    // while (sentBytes != segLength) {
    //     int sent = conn_send(state->conn, segment, segLength);
    //     if (sent == 0) {
    //         // connection closed??
    //     }

    //     sentBytes == sent;
    // }

    /* TODO: What if we're sending a big file? */

    int sentBytes = conn_send(state->conn, segment, segLength);

    #if DEBUG
    printf("sentBytes = %d, segLength = %d\n", sentBytes, segLength);
    print_hdr_ctcp(segment);
    printf("---\n\n");
    #endif

    // clean up resources
    free(buf);
    free(segment);
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
    #if DEBUG
    printf("\n---\n");
    print_hdr_ctcp(segment);
    #endif

    // If the ACK flag is turned on, update our seq number.
    if (segment->flags & TH_ACK) {
        state->seqno = ntohl(segment->ackno);
        
        #if DEBUG
        printf("received ackno = %u\n", ntohl(segment->ackno));
        #endif
    }

    // Output the data if there is data in the segment
    // TODO: I'm subtracting the size of the segment without the data from the 
    // received segment's length, to get the length of the data.
    // what do you think about that? It seems to work well for now.
    //
    // Using len instead of segment->len, because the packet can be truncated
    // or padded, and len is the actual length of the segment we received.
    size_t received_data_len = len - sizeof(ctcp_segment_t);
    if (received_data_len > 0) {
        conn_output(state->conn, segment->data, strlen(segment->data));

        // update our ack number
        state->ackno += received_data_len;

        #if DEBUG
        printf("received len = %lu\n", received_data_len);
        #endif

        // construct an ACK segment - only if we receive data.
        ctcp_segment_t *ack_segment = (ctcp_segment_t *) malloc(sizeof(ctcp_segment_t));
        ack_segment->seqno = htonl(state->seqno);
        ack_segment->ackno = htonl(state->ackno);
        ack_segment->len = htons(sizeof(ctcp_segment_t));

        ack_segment->flags = 0;
        ack_segment->flags |= ACK;
        ack_segment->flags = htonl(ack_segment->flags);

        ack_segment->window = htons(MAX_SEG_DATA_SIZE);
        ack_segment->cksum = 0;
        ack_segment->cksum = cksum(ack_segment, sizeof(ctcp_segment_t));

        // TODO: Check for the actual number of bytes send?
        conn_send(state->conn, ack_segment, sizeof(ctcp_segment_t));

        #if DEBUG
        print_hdr_ctcp(ack_segment);
        #endif

        // free the ACK segment
        free(ack_segment);
    }
    
    #if DEBUG
    printf("---\n\n");
    #endif

    // free resources
    free(segment);
}

void ctcp_output(ctcp_state_t *state) {
    printf("we outputing?\n");
}

void ctcp_timer() {
    /* FIXME */
}
