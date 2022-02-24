// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// This module contains the pest grammar for AT commands.
use pest_derive::Parser;

#[derive(Parser)]
#[grammar_inline = r##"

input = { SOI ~ command ~ EOI }

command = { read | test | execute }

read =    { "AT" ~ optional_extension ~ command_name ~ "?" }
test =    { "AT" ~ optional_extension ~ command_name ~ "=?" }
execute = { "AT" ~ optional_extension ~ command_name ~ delimited_arguments? }

optional_extension = { "+"? }

delimited_arguments = { optional_argument_delimiter ~ arguments ~ optional_argument_terminator }
optional_argument_delimiter = { ("=" | ">")? }
optional_argument_terminator = { ";"? }

arguments = { parenthesized_argument_lists | argument_list? }
parenthesized_argument_lists = { ("(" ~ argument_list ~ ")")+ }
argument_list = { ((argument ~ ",")* ~ argument)? }
argument = { key_value_argument | primitive_argument }
key_value_argument = { primitive_argument ~ "=" ~ primitive_argument }
primitive_argument = @{ (!(WHITE_SPACE | "," | "=" | ">" | ")" | ";") ~ ASCII)* }

command_name = @{ ASCII_ALPHA_UPPER+ }

WHITESPACE = _{ WHITE_SPACE }
COMMENT = _{ "#" ~ (!NEWLINE ~ ANY)* ~ NEWLINE }

"##]

pub struct Grammar {}
