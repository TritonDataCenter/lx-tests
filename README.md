# Tests for the lx brand.

- repo: <git@github.com:joyent/lx-tests.git>

This repo is used for testing lx branded zone emulation. The testing is
intended to be performed while running inside an lx-branded zone.

# Building

If necessary, first install 'make' and the C development packages (compiler
and headers).

cd into the src directory and run 'make'.

# Configuring the tests

Some tests require zone-specific configuration data to run (e.g. the NFS tests
require NFS server information). Test configuration is done via environment
variables. There is an example configuration file named 'src/conf.example'.
Each possible environment variable is shown in this file. You can copy this
file to 'src/conf' and edit it to provide the necessary configuration to test
everything inside your zone. Tests that require configuration are optional and
will be skipped if the relevant environment variables are not set.

# Running the tests

Some OS components might first need to be installed. For example, if you
want to test NFS, some distros require additional packages to be installed.

If necessary, source the configuration file into your shell.

cd into the src directory and run 'make test'. You can run as either a regular
user or as root. Some tests will be skipped if not running as root.

# Writing tests

There are utility functions within the src/util.c file for logging when a test
is skipped, passes or fails. A test should exit non-zero for failure and zero
for success.

If the new tests must be configured, be sure to update the 'src/conf.example'
file to show examples of the entries that can be used. The 'src/mount_nfs.c'
test case can be used as an example for how to handle configuration.
