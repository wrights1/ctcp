### Quan Tran & Steven Wright
### March 1, 2018

# ctcp_README


## 1. Program Structure and Design

### a. Helper functions

We have a few functions to minimize code repetition, making the code more 
readable and coding faster.

- `ctcp_send`: Takes a state and a segment, and sends the given segment over the
connection in the given state.

- `make_segment`: Takes a state, a `char *` buffer and a collection of TCP flags
(in network byte order), and creates a new segment with the given data (if any) 
and the given flags.

- `verify_cksum`: Takes a segment and verifies it using its checksum.

- `convert_to_host_order`: Takes a segment and converts its fields to host byte 
order.

- `convert_to_network_order`: Takes a segment and converts its fields to network 
byte order.

### b. Main functions.

1. `ctcp_state_t`

    We added a few fields to the `ctcp_state_t` struct to store necessary 
    information for the current connection.

    - `seqno`: Our sequence number - the sequence number of the next
    byte we are sending.

    - `ackno`: Our acknowledgement number - the sequence number of the 
    next byte we are expecting from the other side.

    - `cfg`: A pointer to a config struct. The only field we use
    from this struct is `rt_timeout`, in order to determine when a segment has
    timed out.

    - `sent`: A pointer to the last segment we sent, which is 
    used to retransmit the segment if needed, and to free the segment once it
    has been acknowledged.

    - `timeSent`: The timestamp at which the last segment was sent, used
    to determine if that segment has timed out or not.

    - `retransCount`: A counter for the number of retransmission. The 
    program disconnects if `retransCount` reaches 5.

    - `finSent`: A boolean value of 1 if we have sent a FIN, and of 0 if
    we haven't.

    - `finSentAcked`: A boolean value of 1 if we have sent a FIN and 
    the FIN has been ACK'd, and of 0 otherwise.

    - `finRecv`: A boolean value of 1 if we have received a FIN, of 0
    if we haven't.

    - `inputSize`: The size of the last buffer we got from STDIN through
    `conn_input`, used to determine the size of the outgoing segment, and to 
    verify the incoming ACK number.

    - `output_data`: A buffer containing the data to be outputted through
    `ctcp_output`.

    - `received_data_len`: The size of the data received in `ctcp_receive()`.
    This is compared to the return value of `conn_bufspace()`, to determine if 
    there is enough space to output the data or not.

2. `ctcp_init`

    In `ctcp_init`, we initialize the sequence number and ACK number to 0, save
    the `conn` struct and `cfg` struct, and allocate memory for `output_data`.

3. `ctcp_destroy`

    In `ctcp_destroy`, we free the given `cfg` struct, the allocated memory
    for `output_data`, and the `state` itself, before call `end_client()` to 
    exit the program if it's running as a client.

4. `ctcp_read`

    There are 3 cases in `ctcp_read`.

    - If we already sent a FIN, then the function returns without doing 
    anything.

    - Otherwise, we read from STDIN. If an EOF is read, we send a FIN and output
    "EOF".

    - If the input data is not EOF, we send it over the current connection
    normally. We don't update our sequence number immediately, instead we only
    update it once we receiving a proper ACK.

5. `ctcp_receive`

    We first verify for the segment's validity using its checksum. The function
    will return immediately if the check fails, otherwise, the segment's fields
    are converted to host byte order to be used later.

    We then go over the segment to check for signs of a shutdown process.

    - First, if the segment has an ACK flag and we already sent a FIN, we mark
    our FIN as being ACK'd.

        Furthermore, if we already received a FIN, then it means both sides want
        to close down the connection. Thus we call `ctcp_destroy` to terminate
        the connection and close the program.

    - Secondly, if we receive a FIN, we update our ACK number if we haven't yet 
    received any FIN segment, and we acknowledge it.

        If we already sent a FIN and it has been ACK'd, we terminate the 
        connection right away.

    The next 2 cases are when the segment contains the ACK flag, and when it 
    contains data.

    - If the segment has the ACK flag, we compare its acknowledgement number
    to the expected acknowledgement number - which is the current sequence 
    number plus the previously sent segment's data size.

        If they are equal, we update our sequence number accordingly, and reset
        the retransmission counter.

    - If the incoming segment contains data, we first check if we have enough 
    space for outputting by using `conn_bufspace()`. We do nothing if there
    isn't enough space.

        If there is, we first compare the segment's sequence number to our 
        ACK number. If the sequence number is greater than or equal to the ACK 
        number, this means it's a new data segment. We then update our ACK 
        number (because this is stop-and-wait), and output the data using
        `ctcp_output`.

        In any cases, we reply with an ACK (if there is data in the received 
        segment).

6. `ctcp_output`

    In `ctcp_output`, we first check if there is enough space by calling 
    `conn_bufspace`. If there is, we output the data stored in the `state` 
    struct.

7. `ctcp_timer`

    We iterate through the `state_list` linked list. For each `state`, we check
    if something is being sent and waiting to be ACK'd. If there is:

    - If we have retransmitted more than 5 times, we call `ctcp_destroy` to 
    disconnect and close the program.

    - Otherwise, we compare the time from the moment we sent the segment until
    the current time to the timeout value (`rt_timeout`). If it's greater then
    a timeout happened, in which case we retransmit the segment and increase the
    retransmission counter by 1.



## 2. Implementation Challenges

- Teardown sequence.
  - There were many cases that we had to account for after discovering them 
  through testing and trial and error.
  - Had to add a few flags to the state object to keep track of whether or not
  the FIN had been sent from either side, whether that FIN had been ACKd, and
  whether a FIN had been received.
  - Within all of these cases, we had to include additional checks to ensure
  the ACKs we were sending/receiving were in response to the FINs we were 
  sending/receiving.
  - Basically, our experience implementing teardown was very case by case and
  involved lots of trial and error, which was probably a result of improper
  planning but still definitely could have been worse.

- Binary data.
  - Initially used lots of string methods (`strlen`, `strcpy`) to compute
  length of data in several places and to copy data into segments.
  - Once we tested sending binary data we realized these would not be viable
  solutions because binary data contains many empty bytes which, in the context 
  of strings, are interpreted as null terminators, which means all the string 
  methods will return incorrect results.
  - To overcome this, we stopped using `strlen` and instead used the number of 
  bytes read in from stdin as returned by `conn_input` to compute the size of 
  the data.
  - We also used `memcpy` instead of `strcpy` because that method is agnostic to 
  the contents of the memory it copies.
  - Once we did this, we were able to send the reference solution executable from
  out client to our server with no errors.

- `stderr` printing problem.
  - We ran into an unfortunate problem which cost a considerable amount of time
  to hunt down, all because of our debug statements.
  - `conn_output` writes the received data from the network to `stdout`, which 
  is where all our debug statements were also being written to.
  - This caused every one of our debug statements to be written into the 
  received data, which made the file we were piping our output into way too big 
  and full of garbage, totally different from the reference file we were 
  comparing it to.
  - This led us to believe our program was outputting way more segments than it
  should have been, which resulted in us tearing through our entire program 
  trying to find problems which didn't exist.
  - We resolved this by changing every one of our `printf` statements to 
  `fprintf` in order to print to `stderr`, which then showed our algorithms 
  were working completely correctly the entire time.
  - So even though all our cTCP algorithms were right all along, this oversight
  caused us a lot of frustration.

## 3. Testing

We tested our program using the following cases:

- Sending short messages from both sides.

- Sending long text: We send a large text file (larger than `MAX_SEG_DATA_SIZE`)
and use `diff` to compare the received file with the original file.

- Sending a binary file: We send the reference file and use `diff` to compare
the received file with the original reference file.

- Sending over an unreliable network: corrupt, delay, duplicate and drop - 
for all the above 3 cases (short messages, long text, and binary files).

    We do the above 3 test cases again but on an unreliable network. We enable
    corrupt, delay, duplicate and drop one at a time, and finally all at one.

- Using the Python tester file.

Over the course of working on the project, we use the reference file extensively
. 

We first use it as the server, and once we feel comfortable, we switch to 
using our program as the server and reference as the client, before using our
program as both the client and the server.

We also make use of the logging feature, especially to compare the checksums and 
verify the presence of the segments.

Lastly, we use `valgrind` to check for memory leaks when running the program as
both the client and server. `valgrind` reports no memory leak for all of our
test cases.


## 4. Remaining Bugs

We currently have no knowledge of any remaining bugs.