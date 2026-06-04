# Debugging with LLDB

This guide explains how to debug BGP using LLDB, though the techniques used here
are fairly generic and may apply to other binaries as well.

## Overview

There are two cases covered in this document:

1. **Debugging core files** - Analyzing crash dumps after the fact
2. **Debugging running binaries** - Live debugging on the switch

> **Note:** This guide covers practical debugging techniques. For a
> comprehensive LLDB tutorial, please refer to the official LLDB documentation.

---

## Prerequisites

Before debugging, you need to know:

- **Binary name with version** that needs to be debugged
  - Example: `neteng.fboss.wedge_bgpd:444`

- **Debug-info file** that aligns with this binary version
  - ⚠️ **Important:** The version number of the corresponding debug-info is
    **NOT necessarily the same** as the binary version

### Finding the Correct Debug-Info Version

To find the `neteng.fboss.wedge_bgpd.debuginfo` package version corresponding to
the binary, run this script:

```bash
BGPD_VERSION=444  # your version here
fbpkg versions neteng.fboss.wedge_bgpd.debuginfo --show-revision | grep $(fbpkg info --format json neteng.fboss.wedge_bgpd:$BGPD_VERSION | jq -r ".build.vcs_info.revision") | awk '{print $1;}'
```

---

## Common Setup (Required for Both Cases)

On your devvm, fetch the binary and debug-info packages:

```bash
mkdir test
cd test
fbpkg fetch neteng.fboss.wedge_bgpd:444
fbpkg fetch neteng.fboss.wedge_bgpd.debuginfo:434
# In this example, 434 is the corresponding debuginfo version for prod version 444
```

---

## Case 1: Debugging Core Files

Use this approach when analyzing crashes after they have occurred.

### Step 1: Retrieve the Core File

```bash
# SSH into the switch
sush2 <switch>

# Find the core file under /var/tmp/cores
ls /var/tmp/cores
# Example core files: /var/tmp/cores/bgpd_main

# Exit from the switch
exit
```

### Step 2: Copy the Core File to Your DevVM

```bash
suscp2 --reason debug root@<switch>:/var/tmp/cores/<core-file-name> .
```

### Step 3: Load the Core File in LLDB

```bash
lldb
```

```lldb
(lldb) file cpp/bgpd_cpp -core <path to core file name>
(lldb) target symbols add cpp/bgpd_cpp.debuginfo
```

---

## Case 2: Debugging Running Binary on the Switch

> ⚠️ **Warning:** Running lldb on production switch should be done **only if it
> is necessary** to resolve a critical issue on the switch. Accidental
> operations (even simply pausing/stopping a thread to look at data) using lldb
> can affect the function of the binary.

### Step 1: Prepare the Switch Environment

```bash
# SSH into the switch
sush2 <switch>

# Create a working directory
mkdir /tmp/<user-id>
chmod +rw /tmp/<user-id>

# Exit from the switch
exit
```

### Step 2: Copy Debug Files to the Switch

```bash
suscp2 -r test/ <switch>:/tmp/<user-id> --reason debug
```

### Step 3: Attach LLDB to the Running Process

```bash
# SSH back into the switch
sush2 <switch>

# Get the PID of the binary
ps -aef | grep bgpcpp

# Navigate to your working directory
cd /tmp/<user-id>

# Start LLDB with sudo
sudo lldb
```

```lldb
(lldb) attach <PID>
(lldb) target symbols add test/cpp/bgpd_cpp.debuginfo
```

---

## Example LLDB Session

Below is an example LLDB debugging session demonstrating common commands:

### Loading Debug Symbols

```lldb
(lldb) target symbols add cores/cpp/bgpd_cpp.debuginfo
symbol file '/tmp/svshah/cores/cpp/bgpd_cpp.debuginfo' has been added to '/etc/packages/neteng-fboss-bgpd/e04f84efad770f126f6a93e1a0211e87/cpp/bgpd_cpp'
```

### Viewing the Backtrace

```lldb
(lldb) bt
* thread #1, name = 'bgpd_main', stop reason = signal SIGSTOP
  * frame #0: 0x00007ff05c494101
    frame #1: 0x00007ff05c499583
    frame #2: 0x00007ff05c0df55f
    frame #3: 0x0000000002c001ee bgpd_cpp`main(argc=1, argv=0x00007ffd62fa1c08) at Main.cpp:635
    frame #4: 0x00007ff05c42c657
    frame #5: 0x00007ff05c42c718
    frame #6: 0x0000000002bf9a21 bgpd_cpp`_start at start.S:116
```

### Selecting and Inspecting Frames

```lldb
(lldb) frame select 3
frame #3: 0x0000000002c001ee bgpd_cpp`main(argc=1, argv=0x00007ffd62fa1c08) at Main.cpp:635

(lldb) print argv[0]
(char *) 0x00007ffd62fa3834 "/etc/packages/neteng-fboss-bgpd/current/cpp/bgpd_cpp"
```

### Listing Threads

Use `thread list` to see all threads in the process:

```lldb
(lldb) thread list
Process 3245994 stopped
* thread #1: tid = 3245994, 0x00007ff05c494101, name = 'bgpd_main', stop reason = signal SIGSTOP
  thread #2: tid = 3245998, 0x00007ff05c494101, name = 'jemalloc_bg_thd', stop reason = signal SIGSTOP
  thread #3: tid = 3246010, 0x00007ff05c520e89, name = 'FutureTimekeepr', stop reason = signal SIGSTOP
  ... (additional threads)
  thread #47: tid = 3246137, 0x00007ff05c520e89, name = 'peer_manager', stop reason = signal SIGSTOP
  ... (additional threads)
```

### Switching Between Threads

```lldb
(lldb) thread select 47
* thread #47, name = 'peer_manager', stop reason = signal SIGSTOP
    frame #0: 0x00007ff05c527ca2
->  0x7ff05c527ca2: cmpq   $-0x1000, %rax ; imm = 0xF000
    0x7ff05c527ca8: ja     0x7ff05c527ce0
    0x7ff05c527caa: movl   %r8d, %edi
    0x7ff05c527cad: movl   %eax, -0x4(%rbp)
```

### Viewing Thread Backtrace

```lldb
(lldb) bt
* thread #47, name = 'peer_manager', stop reason = signal SIGSTOP
  * frame #0: 0x00007ff05c527ca2
    frame #1: 0x0000000002e15f93 bgpd_cpp`epoll_dispatch(base=<unavailable>, arg=0x00007ff05a127360, tv=<unavailable>) at epoll.c:317:8
    frame #2: 0x0000000002e144ea bgpd_cpp`event_base_loop(base=0x00007ff059c2a200, flags=<unavailable>) at event.c:524:9
    frame #3: 0x0000000002e0810d bgpd_cpp`folly::EventBase::loopMain(this=0x00007ffd62fa0d80, flags=0, options=...) at EventBase.cpp:0
    frame #4: 0x0000000002e077aa bgpd_cpp`folly::EventBase::loopBody(this=0x00007ffd62fa0d80, flags=0, options=...) at EventBase.cpp:513
    frame #5: 0x0000000002e0774c bgpd_cpp`folly::EventBase::loop(this=0x00007ffd62fa0d80) at EventBase.cpp:474
    frame #6: 0x000000000b3586b4 bgpd_cpp`facebook::bgp::PeerManager::run(this=0x00007ffd62fa1040) at PeerManager.cpp:293
    ...
```

### Inspecting Variables

```lldb
(lldb) frame select 6
frame #6: 0x000000000b3586b4 bgpd_cpp`facebook::bgp::PeerManager::run(this=0x00007ffd62fa1040) at PeerManager.cpp:293

(lldb) print workers_
(std::vector<folly::Future<folly::Unit>, std::allocator<folly::Future<folly::Unit> > >) size=2 {
  [0] = {
    folly::futures::detail::FutureBase<folly::Unit> = {
      core_ = 0x00007ff04ee1ae80
    }
  }
  [1] = {
    folly::futures::detail::FutureBase<folly::Unit> = {
      core_ = 0x00007ff04ee1b060
    }
  }
}
```

---

## Quick Reference: Common LLDB Commands

| Command                           | Description                       |
| --------------------------------- | --------------------------------- |
| `file <binary> -core <core-file>` | Load a binary with a core file    |
| `target symbols add <debuginfo>`  | Add debug symbols                 |
| `attach <PID>`                    | Attach to a running process       |
| `bt`                              | Show backtrace for current thread |
| `thread list`                     | List all threads                  |
| `thread select <N>`               | Switch to thread N                |
| `frame select <N>`                | Select stack frame N              |
| `print <variable>`                | Print a variable's value          |
| `continue`                        | Resume execution                  |
| `step`                            | Step into next line               |
| `next`                            | Step over next line               |
| `breakpoint set -n <function>`    | Set breakpoint on function        |
| `breakpoint list`                 | List all breakpoints              |
| `quit`                            | Exit LLDB                         |

---

## See Also

- [LLDB Official Documentation](https://lldb.llvm.org/)
- [Network Routing Team Wiki](https://www.internalfb.com/wiki/Net_Systems/Teams/Network_Routing/)

---

_Source:
[Debugging with LLDB Wiki](https://www.internalfb.com/wiki/Net_Systems/Teams/Network_Routing/Debugging_with_LLDB/)_
