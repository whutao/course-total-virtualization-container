#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>


#define COMMAND_BUFFER_SIZE 256
#define STACK_SIZE 30 * 1024 * 1024
#define CLONE_FLAGS CLONE_NEWNS | \
                    CLONE_NEWUTS | \
                    CLONE_NEWPID | \
                    CLONE_NEWIPC | \
                    CLONE_NEWNET


static char child_stack[STACK_SIZE];


// Helper

void create_peer_interfaces() {
    system("ip link add veth0 type veth peer name veth1");
    system("ip link set veth0 up");
    system("brctl addif br0 veth0");
}

void unmount_proc() {
    umount2("/proc", MNT_DETACH);
}

void mount_proc() {
    mount("proc", "/proc", "proc", 0, NULL);
}

void setup_network_routes() {
    system("ip link set veth1 up");
    char ip_addr_add_command[COMMAND_BUFFER_SIZE];
    snprintf(
        ip_addr_add_command,
        sizeof(ip_addr_add_command),
        "ip addr add 172.16.0.101/24 dev veth1"
    );
    system(ip_addr_add_command);
    system("route add default gw 172.16.0.100 veth1");
}

void setup_root_directory() {
    mount("./rootfs", "./rootfs", "bind", MS_BIND | MS_REC, "");
    mkdir("./rootfs/oldrootfs", 0755);
    syscall(SYS_pivot_root, "./rootfs", "./rootfs/oldrootfs");
    chdir("/");
    umount2("/oldrootfs", MNT_DETACH);
    rmdir("/oldrootfs");
}


// Container

int container_routine(void *args) {

    // Disassociate mount namespace
    unshare(CLONE_NEWNS);

    // We don't want to access the processes from the host OS.
    unmount_proc();

    // We use a "pivot_root". Moves an existing rootfs
    //  into a subdirectory, and make a another directory a new root.
    setup_root_directory();

    // Mount "/proc" back.
    mount_proc();

    // Set a unique container process name.
    sethostname("whutao", 6);

    // Configure the network routes.
    setup_network_routes();

    // Execute command in the child process.
    char **argv = (char **)args;
    execvp(argv[0], argv);

    return EXIT_SUCCESS;

}


// Main

int main(int argc, char *argv[]) {
    
    // Create a pair of peer interfaces, veth0 and veth1, link
    //  them to the br0 and set up routing within the container.
    create_peer_interfaces();

    // Create a child process for the container.
    int container_pid = clone(
        container_routine,
        child_stack + sizeof(child_stack),
        CLONE_FLAGS | SIGCHLD,
        argv + 1
    );

    // PID is negative if the cloning failed.
    if (container_pid < 0) {
        fprintf(stderr, "Cannot clone due to error code %d.\n", errno);
        return EXIT_FAILURE;
    }
    
    // Add veth1 to the new child namespace.
    char ip_link_set_command[COMMAND_BUFFER_SIZE];
    snprintf(
        ip_link_set_command,
        sizeof(ip_link_set_command) - 1,
        "ip link set veth1 netns %d",
        container_pid
    );
    system(ip_link_set_command);

    // Wait until the child terminates.
    waitpid(container_pid, NULL, EXIT_SUCCESS);

    return EXIT_SUCCESS;

}
