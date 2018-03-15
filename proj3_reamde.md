### Quan Tran & Steven Wright
### March 1, 2018

# ctcp_README - Sliding Window Edition

## 1. Program Structure and Design

 We will only include here the things that have been created or modified in Project 3

### a. Helper functions & structs

- `typedef struct segment_info_t`: In Project 2, we kept track of the
retransmission count and the time sent in `state` because there was only one
segment out at a time, which allowed those things to be globally defined. With
sliding window, we had to keep track of these things per segment, so we created
a struct that would hold a segment and some necessary associated information.

- `make_segment_info`: Similar to the make_segment function, but takes a segment
 as an argument and returns an initialized segment_info_t type.

- `ctcp_send`: Takes a state and a segment_info_t returned by make_segment_info,
and sends the segment within the segment_info_t over the connection in the
given state.

### b. Main functions & structs

1. `ctcp_state_t`

    We added mode fields to the `ctcp_state_t` struct to store more necessary
     information that pertains to sliding window  

    - `uint32_t send_base`: lowest byte number within the send window, used to
     move send window along as data is sent and ACKd properly. Is maintained to
     be the highest byte number of contiguous data received.


    - `uint32_t next_byte_consume`: keeps track of what data we have output so far.
    This is used in `ctcp_output` to avoid re-outputting data.

    - `linked_list_t *sent`: linked list containing `segment_info_t` objects
    that have been sent and not yet acknowledged. Segments are removed as they
    are ACKd. This is represents the sliding send window.

    - `linked_list_t *received`: linked list containing segments that have been
    received but not yet outputted. Segments are removed as they are output.
    This represents the sliding receive window.

    - `uint16_t send_window_avail`: number of bytes currently available in the
    send window. Initialized to be the window size as specified by the user with
     the `-w` flag. Used to properly fill the send window with data and keep
     track of how much the sender has already sent, and how much it can send next.

    - `uint16_t recv_window_avail`: number of bytes currently available in the
    receive window. Initialized to be the window size as specified by the user
     with the `-w` flag. Used to determine how much data has been received and
      how much it can still take.

    - `uint16_t advertised_window_size`: receiver window size as advertised in
    the window field of the previously received ACK's TCP header, used to ensure
     the sender has room to send data at all.

2. `ctcp_init`

    Here we initialize all the previously discussed fields in `state` and create
     the linked lists for `sent` and `receive`.

3. `ctcp_read`
    There are three main cases in read:
    - If we have already sent a FIN, it returns as nothing else can be sent
    - If we haven't and there is space in the send window, we read in the data.
        - If we read an EOF we send a FIN and output "EOF".
        - While not reading EOF, we create as many segments as are needed to
        contain the data and add them all to the `sent` linked list. We then
        traverse the list and send as many segments (that have not already
        sent) as `advertised_window_size` will allow. We keep track of
        whether or not a segment has been previously sent with a flag
         variable within the `segment_info_t` struct.
    - If there is no space in the send window, we return and wait until there is

4. `ctcp_receive`
    We first verify the checksum and go about the teardown process as we did in
    the previous project. The only differences here are that we manage the memory
    differently. The two main cases are whether we are receiving an ACK or a
    segment with data in it.
    - If receiving an ACK, and there are segments in `sent` (unacked data), there
    are two cases.
        - If the ACK is acking contiguous data (ackno > send_base), we update
        `send_base` to the new `ackno`, traverse `sent` and remove all segments
        up to the new `send_base`, effectively moving the send window forward as
         the sender receives ACKs for successfully contiguously sent data
        -  If the ACK is acking non-contiguous data (ackno = send_base) we know
        there is gap, so we do nothing and wait for retransmission.
    - If receiving data, and there is room in the receive window, there are again
    two cases
        - If the received segment's seqno = the current ackno then we know the data is
        contiguous so we put the segment at the front of `received` and traverse
        `received` to update the state ackno to the new highest contiguous byte,
         and then call `ctcp_output` to output the data.
        - If it is not contiguous, then we must find the correct place for the
        out of order data in `received` so it will all be output in the correct
        order. We do this by traversing the linked list and putting the segment
        where it's sequence number is greater than the previous segment and
        lesser than the next. We only do this if we do not already have the
        segment in the linked list, which we check for before finding its place.

    We then make and send an ACK no matter what, even if the window is full. We
    do this because it needs to update `advertised_window_size` in the sender.
    This is how the sender knows to stop sending if the window is full.

5. `ctcp_output`

    We traverse `received`, outputting each segment with `conn_output` and
    removing it from the linked list, until `received` is empty or we hit a gap.
    We increment `next_byte_consume`and `recv_window_avail` each time, moving
     the receive window forward. We check if there is enough space each time with
     `conn_bufspace` and we break the loop if there is not.

6. `ctcp_timer`

    This is effectively the same as the timer from Porject 2, but the entire
    thing is within a loop that iterates through the list of `state`s, and also
     traverses the linked list of `sent` to check each timestamp, rather than
     just comparing a single global timestamp as we had in the previous project.

## 2. Implementation Challenges

- Understanding protocol

    Our first challenge was simply understanding exactly how exactly selective repeat
    with cumulative ACKs works and what it means for our implementation. We used
    the selective repeat animation to clarify exactly what we needed to achieve
    and how certain cases were supposed to be handled. Then we moved on to actually
    implementing it.

- Managing and moving windows on both sides

    We spent a considerable amount of time fleshing out how we stored and
    maintained the send window. We struggled with implementing a good way to
    correctly take in the ACKs when there were gaps and moving the window forward
    when there was contiguous data. implementing the send functionality took us
    the longest because we were figuring out how the protocol behaved in all cases
    while developing. Once we started keeping track of the boundary between
    contiguous and non-contiguous data with `send_base`, it made much more sense.
    Doing the same for the receive window had its own challenges but it came
    easier than send because we had done that first. The struggle with the receive
    window was determining the best way to keep data in order and fill gaps when
    they occur. We had a moderately big problem updating the ackno to the new contiguous data  
    boundary after filling a gap, but solved it by traversing the linked list.


- Filling gaps when receiving data

    As noted, we had trouble devising a good way to take in potentially out of order
    data and put it in the right places. At first there were so many cases that
    it seemed incredibly hard and confusing. The more we thought about it and
    worked it out, we realized there are really only 2 cases with a few sub-cases
    each. Either the data is the at the expected position (contiguous) in which
    case the data is outputted and the window just moves forward, or the data is
    non-contiguous and we must find where it goes by looking at the seqno's of
    the segments currently in the window. It actually didn't take very long to
    code the receiver but it took a lot of thinking and designing.

- Unreliability

    With sliding window, there is far more opportunity for error when sending
    over an unreliable network. In the last project we only had to worry about
    one segment making there and getting an ACK back, which is a world of difference
    compared to sliding window. Sifting through log files to find the one segment
    that didn't make it or wasn't ACKed correctly or fell outside the window and
    identify the causes of that error was painful and there were so many cases
    to account for. We had several errors we discovered because of unreliability.
    There were instances on both the sender and receiver sides where we had errors
    related to incorrectly updating values in special cases like getting out of
    order segments/ACKs that only manifested themselves under unreliable network
    testing.

## 3. Testing

The only thing in our testing procedure that differs from Project 2 was playing with
window size. We used mismatch in window sizes between client and server to test
our handling of full windows on both sides. For example, setting the client
window to three times the size of the server window will constantly test our case
 in which the sender needs to be flow controlled.

## 4. Remaining Bugs

There have to be bugs but we haven't discovered them yet.
