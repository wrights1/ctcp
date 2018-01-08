CC = gcc
CFLAGS = -g -Wall -Werror -pthread

LAB=lab12
TAR = ctcp.tar.gz
SUBMISSION_SITE = https://notebowl.denison.edu

# Add any header files you've added here.
HDRS = ctcp_linked_list.h ctcp_utils.h ctcp.h ctcp_sys.h ctcp_sys_internal.h
# Add any source files you've added here.
SRCS = ctcp_linked_list.c ctcp_utils.c ctcp.c ctcp_sys_internal.c
OBJS = $(patsubst %.c,%.o,$(SRCS))
DEPS = $(patsubst %.c,.%.d,$(SRCS))

.PHONY: all clean submit

all: ctcp

$(OBJS): %.o : %.c
	$(CC) -c $(CFLAGS) $< -o $@

$(DEPS): .%.d : %.c
	$(CC) -MM $(CFLAGS) $<  > $@

ctcp: $(OBJS)
	$(CC) $(CFLAGS) -o ctcp $(OBJS)

submit: clean
	./.tarSubmission.sh $(TAR) $(LAB)
	@echo
	@echo
	@echo '  Your submission file $(TAR) was successfully created.'
	@echo '  Please submit it to the following URL: '
	@echo '   $(SUBMISSION_SITE)'
	@echo

clean:
	rm -f .*.d *.o $(TAR) *~ ctcp
