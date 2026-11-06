CC = cc
CFLAGS = -std=c11 -O2 -g -Wall -Wextra -Werror -pedantic
PROGRAMS = local_add fork_private thread_shared fork_status fork_failure socket_rpc
SYSCALL_PROGRAMS = errno_write interrupt_wait
MODEL_PROGRAMS = command_dispatch request_queue transactions nodes references
BINARIES = $(addprefix build/,$(PROGRAMS) $(SYSCALL_PROGRAMS) $(MODEL_PROGRAMS)) build/mapping build/parcel

.PHONY: all test kernel busybox rootfs vm-test vm-test-kvm vm-runner-test clean
all: $(BINARIES)

build:
	mkdir -p $@

build/%: examples/intro/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@

build/thread_shared: examples/intro/thread_shared.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $< $(LDFLAGS) -o $@

$(addprefix build/,$(SYSCALL_PROGRAMS)): build/%: examples/syscalls/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@

$(addprefix build/,$(MODEL_PROGRAMS)): build/%: examples/models/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@

build/mapping: examples/memory/mapping.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) -o $@

build/parcel: examples/models/parcel.c userspace/parcel.c userspace/parcel.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iuserspace examples/models/parcel.c userspace/parcel.c $(LDFLAGS) -o $@

test: all
	python3 -m unittest discover -s tests -v

kernel:
	bash tools/kernel/build.sh

busybox:
	bash tools/qemu/build-busybox.sh

rootfs:
	bash tools/qemu/build-rootfs.sh

vm-test: rootfs
	bash tools/qemu/run.sh

vm-test-kvm: rootfs
	ACCEL=kvm bash tools/qemu/run.sh

vm-runner-test: rootfs
	bash tools/qemu/test-runner.sh

clean:
	rm -f $(BINARIES) build/device_test build/rpc_test build/add_server build/add_client
