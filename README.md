# Tests for the lx brand.

- repo: <git@github.com:joyent/lx-tests.git>

This repo is used for testing lx branded zone emulation.

# Building

cd into the src directory and run 'make'.

# Running the tests

cd into the src directory and run 'make test'.

# Writing tests

There are utility functions within the src/util.c file for logging when a test
passes or fails. A test should exit non-zero for failure and zero for success.
