# CSE506
CSE506, Operating Systems with Prof. Ferdman

# Warm-up Project 1 (Part 1 of 2)

```bash
# 1. Build succeeds with -Wall -Werror (already confirmed)
make

# 2. sbush is a valid RISC-V ELF
file build/rootfs/bin/sbush

# 3. It disassembles correctly (has real code, not just stubs)
riscv64-unknown-elf-objdump -d build/rootfs/bin/sbush | head -40

# 4. echo, cat, ls all build (no libc regression)
file build/rootfs/bin/echo
file build/rootfs/bin/cat
file build/rootfs/bin/ls

# 5. sbush is packed into the tarfs
tar tf build/rootfs.tar | grep sbush

# 6. Key symbols are present (main, execute_line, parse_line, etc.)
riscv64-unknown-elf-nm build/rootfs/bin/sbush | grep -E 'main|parse_line|execute_pipeline|env_init'
```

## `cd`

Start by running:
```bash
qemu-riscv64 build/rootfs/bin/sbush
```

May need to run `sudo apt install qemu-user`.

**NOTE**:
- `qemu-system-riscv64` emulates a full RISC-V machine (CPU, RAM, devices) and boots a kernel — needs an OS to do anything
- `qemu-riscv64` (user-mode) runs a single RISC-V binary on your host Linux kernel, which handles syscalls directly — no OS needed

You should then see:
```sh
sbush$
```

You should be able to navigate to /tmp:
```bash
cd /tmp
```

## `echo`

Inside the same `sbush`, type (or copy the commands below):

```bash
export PATH=~/CSE506/sbunix-skeleton/build/rootfs/bin

echo Hello World
```

## `cat`

```bash
export PATH=~/CSE506/sbunix-skeleton/build/rootfs/bin

# Read a file
cat /etc/hostname

# Read from stdin (Ctrl-D to end)
cat
```

## `ls`

```bash
export PATH=~/CSE506/sbunix-skeleton/build/rootfs/bin

ls /tmp
ls /home
```

## Execute binaries interactively

See [`echo`](#echo).

## Execute scripts

From a different shell (not `sbush`) run:

```bash
# Create a test script
cat > /tmp/test.sh << 'EOF'
#!/sbush
echo hello from script
echo second line
EOF
```

Then run:

```bash
# Run it (set PATH so echo is found)
qemu-riscv64 -E PATH=~/CSE506/sbunix-skeleton/build/rootfs/bin build/rootfs/bin/sbush /tmp/test.sh
```

NOTE: The `-E` flag sets environment variables for the emulated binary, so you don't need to export PATH inside the script.

## Set and use PATH and PS1 variables

Start the `sbush`:
```bash
qemu-riscv64 build/rootfs/bin/sbush
```

Then type:
```bash
export PS1=myshell$
export PATH=~/CSE506/sbunix-skeleton/build/rootfs/bin
echo this should work
set FOO bar
```

After setting `PS1`, the prompt should change from `sbush$` to `myshell$`. After setting `PATH`, `echo` should resolve without needing the full path.

`set` creates shell-only variables (not passed to child processes), while export makes them visible to executed programs.

## Background Processes

In the same `sbush` type:

```bash
echo hello &
```

## Pipes

```bash
export PATH=~/CSE506/sbunix-skeleton/build/rootfs/bin

echo hello | cat
echo hello | cat | cat
```
