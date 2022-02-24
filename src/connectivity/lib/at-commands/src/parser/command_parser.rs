// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// The functions in this module convert the untyped pest Pair syntax nodes to typed AT command AST nodes.
///
/// These functions should not fail, but report failures from the next_* methods if the
/// expected element is not found.  This should only happen if the code here does not correctly
/// match the parse tree defined in command_grammar.rs.
use {
    crate::{
        lowlevel::Command,
        parser::{
            arguments_parser::ArgumentsParser,
            command_grammar::{Grammar, Rule},
            common::{next_match, next_match_one_of, parse_name, ParseError, ParseResult},
        },
    },
    pest::{iterators::Pair, Parser},
};

static ARGUMENTS_PARSER: ArgumentsParser<Rule> = ArgumentsParser {
    argument_list: Rule::argument_list,
    argument: Rule::argument,
    arguments: Rule::arguments,
    key_value_argument: Rule::key_value_argument,
    optional_argument_delimiter: Rule::optional_argument_delimiter,
    optional_argument_terminator: Rule::optional_argument_terminator,
    parenthesized_argument_lists: Rule::parenthesized_argument_lists,
    primitive_argument: Rule::primitive_argument,
};

pub fn parse(string: &String) -> ParseResult<Command, Rule> {
    let mut parsed = Grammar::parse(Rule::input, string).map_err(|pest_error| {
        ParseError::PestParseFailure { string: string.clone(), pest_error }
    })?;

    let input = next_match(&mut parsed, Rule::input)?;
    let mut input_elements = input.into_inner();
    let command = next_match(&mut input_elements, Rule::command)?;

    parse_command(command)
}

fn parse_command(command: Pair<'_, Rule>) -> ParseResult<Command, Rule> {
    let mut command_elements = command.into_inner();
    let command_variant =
        next_match_one_of(&mut command_elements, vec![Rule::execute, Rule::read, Rule::test])?;

    let parsed_command = match command_variant.as_rule() {
        Rule::execute => parse_execute(command_variant),
        Rule::read => parse_read(command_variant),
        Rule::test => parse_test(command_variant),
        // This is unreachable since next_match_one_of only returns success if one of the rules
        // passed into it matches; otherwise it returns Err and this method will return early
        // before reaching this point.
        _ => unreachable!(),
    }?;

    Ok(parsed_command)
}

fn parse_execute(execute: Pair<'_, Rule>) -> ParseResult<Command, Rule> {
    let mut execute_elements = execute.into_inner();

    let optional_extension = next_match(&mut execute_elements, Rule::optional_extension)?;
    let parsed_optional_extension = parse_optional_extension(optional_extension)?;

    let name = next_match(&mut execute_elements, Rule::command_name)?;
    let parsed_name = parse_name(name)?;

    let delimited_arguments = next_match(&mut execute_elements, Rule::delimited_arguments)?;
    let parsed_delimited_arguments =
        ARGUMENTS_PARSER.parse_delimited_arguments(delimited_arguments)?;

    Ok(Command::Execute {
        name: parsed_name,
        is_extension: parsed_optional_extension,
        arguments: parsed_delimited_arguments,
    })
}

fn parse_read(read: Pair<'_, Rule>) -> ParseResult<Command, Rule> {
    let mut read_elements = read.into_inner();

    let optional_extension = next_match(&mut read_elements, Rule::optional_extension)?;
    let parsed_optional_extension = parse_optional_extension(optional_extension)?;

    let name = next_match(&mut read_elements, Rule::command_name)?;
    let parsed_name = parse_name(name)?;

    Ok(Command::Read { name: parsed_name, is_extension: parsed_optional_extension })
}

fn parse_test(test: Pair<'_, Rule>) -> ParseResult<Command, Rule> {
    let mut test_elements = test.into_inner();

    let optional_extension = next_match(&mut test_elements, Rule::optional_extension)?;
    let parsed_optional_extension = parse_optional_extension(optional_extension)?;

    let name = next_match(&mut test_elements, Rule::command_name)?;
    let parsed_name = parse_name(name)?;

    Ok(Command::Test { name: parsed_name, is_extension: parsed_optional_extension })
}

fn parse_optional_extension(optional_extension: Pair<'_, Rule>) -> ParseResult<bool, Rule> {
    let extension_str = optional_extension.as_span().as_str();

    match extension_str {
        "" => Ok(false),
        "+" => Ok(true),
        c => Err(ParseError::UnknownExtensionCharacter(c.to_string())),
    }
}
