#
# Command module definition for disvm Shell
#

# Sys and Draw modules must be included before this file in modules.

Command: module
{
    init: fn(cxt: ref Draw->Context, args: list of string);
};