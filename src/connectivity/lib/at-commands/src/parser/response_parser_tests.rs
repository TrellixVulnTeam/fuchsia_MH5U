// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// Tests for the AT command response parser.
use crate::{
    lowlevel::{Argument, Arguments, DelimitedArguments, HardcodedError, Response},
    parser::response_parser,
};

#[test]
fn parse_empty() {
    let string = String::from("");
    let parse_result = response_parser::parse(&string);
    assert!(parse_result.is_err());
}

#[test]
fn parse_fail() {
    let string = String::from("unparseable");
    let parse_result = response_parser::parse(&string);
    assert!(parse_result.is_err());
}

fn test_parse(str_to_parse: &str, expected_result: Response) {
    let parse_result = response_parser::parse(&String::from(str_to_parse)).unwrap();
    assert_eq!(expected_result, parse_result);
}

// Ok response
#[test]
fn ok() {
    test_parse("OK", Response::Ok)
}

// Error response
#[test]
fn error() {
    test_parse("ERROR", Response::Error)
}

// Hardcoded error response NO_CARRIER
#[test]
fn no_carrier() {
    test_parse("NO CARRIER", Response::HardcodedError(HardcodedError::NoCarrier))
}

// Hardcoded error response BUSY
#[test]
fn busy() {
    test_parse("BUSY", Response::HardcodedError(HardcodedError::Busy))
}

// Hardcoded error response NO_ANSWER
#[test]
fn no_answer() {
    test_parse("NO ANSWER", Response::HardcodedError(HardcodedError::NoAnswer))
}

// Hardcoded error response DELAYED
#[test]
fn delayed() {
    test_parse("DELAYED", Response::HardcodedError(HardcodedError::Delayed))
}

// Hardcoded error response BLACKLIST
#[test]
fn blacklist() {
    test_parse("BLACKLIST", Response::HardcodedError(HardcodedError::Blacklist))
}

// +CME error
#[test]
fn cme_error() {
    test_parse("+CME ERROR: 1", Response::CmeError(1))
}

// Response with no arguments
#[test]
fn no_args() {
    test_parse(
        "TEST: ",
        Response::Success {
            name: String::from("TEST"),
            is_extension: false,
            arguments: DelimitedArguments {
                delimiter: Some(String::from(":")),
                arguments: Arguments::ArgumentList(vec![]),
                terminator: None,
            },
        },
    )
}

// Response with no arguments and no delimiter
#[test]
fn no_args_no_delimiter() {
    test_parse(
        "TEST",
        Response::Success {
            name: String::from("TEST"),
            is_extension: false,
            arguments: DelimitedArguments {
                delimiter: None,
                arguments: Arguments::ArgumentList(vec![]),
                terminator: None,
            },
        },
    )
}

// Extension response with no arguments
#[test]
fn ext_no_args() {
    test_parse(
        "+TEST: ",
        Response::Success {
            name: String::from("TEST"),
            is_extension: true,
            arguments: DelimitedArguments {
                delimiter: Some(String::from(":")),
                arguments: Arguments::ArgumentList(vec![]),
                terminator: None,
            },
        },
    )
}

// Extension response with no arguments and no delimiter
#[test]
fn ext_no_args_no_delimiter() {
    test_parse(
        "+TEST",
        Response::Success {
            name: String::from("TEST"),
            is_extension: true,
            arguments: DelimitedArguments {
                delimiter: None,
                arguments: Arguments::ArgumentList(vec![]),
                terminator: None,
            },
        },
    )
}

// Extension response with one integer argument
#[test]
fn one_int_arg_no_comma() {
    test_parse(
        "+TEST: 1",
        Response::Success {
            name: String::from("TEST"),
            is_extension: true,
            arguments: DelimitedArguments {
                delimiter: Some(String::from(":")),
                arguments: Arguments::ArgumentList(vec![Argument::PrimitiveArgument(
                    String::from("1"),
                )]),
                terminator: None,
            },
        },
    )
}

// Extension response with one integer argument, no delimiter
#[test]
fn one_int_arg_no_comma_no_delimiter() {
    test_parse(
        "+TEST1",
        Response::Success {
            name: String::from("TEST"),
            is_extension: true,
            arguments: DelimitedArguments {
                delimiter: None,
                arguments: Arguments::ArgumentList(vec![Argument::PrimitiveArgument(
                    String::from("1"),
                )]),
                terminator: None,
            },
        },
    )
}

// Extension response with one string argument
#[test]
fn one_string_arg_no_comma() {
    test_parse(
        "+TEST: abc",
        Response::Success {
            name: String::from("TEST"),
            is_extension: true,
            arguments: DelimitedArguments {
                delimiter: Some(String::from(":")),
                arguments: Arguments::ArgumentList(vec![Argument::PrimitiveArgument(
                    String::from("abc"),
                )]),
                terminator: None,
            },
        },
    )
}

// Extension response with one key-value argument
#[test]
fn one_kv_arg_no_comma() {
    test_parse(
        "+TEST: 1=abc",
        Response::Success {
            name: String::from("TEST"),
            is_extension: true,
            arguments: DelimitedArguments {
                delimiter: Some(String::from(":")),
                arguments: Arguments::ArgumentList(vec![Argument::KeyValueArgument {
                    key: String::from("1"),
                    value: String::from("abc"),
                }]),
                terminator: None,
            },
        },
    )
}

// Extension response with multiple arguments
#[test]
fn args_no_comma() {
    test_parse(
        "+TEST: abc,1",
        Response::Success {
            name: String::from("TEST"),
            is_extension: true,
            arguments: DelimitedArguments {
                delimiter: Some(String::from(":")),
                arguments: Arguments::ArgumentList(vec![
                    Argument::PrimitiveArgument(String::from("abc")),
                    Argument::PrimitiveArgument(String::from("1")),
                ]),
                terminator: None,
            },
        },
    )
}

// Paren delimited argument list
#[test]
fn paren_args() {
    test_parse(
        "+TEST: (1)",
        Response::Success {
            name: String::from("TEST"),
            is_extension: true,
            arguments: DelimitedArguments {
                delimiter: Some(String::from(":")),
                arguments: Arguments::ParenthesisDelimitedArgumentLists(vec![vec![
                    Argument::PrimitiveArgument(String::from("1")),
                ]]),
                terminator: None,
            },
        },
    )
}

// Paren delimited multiple argument lists
#[test]
fn multiple_paren_args() {
    test_parse(
        "+TEST: (1)(2,abc)",
        Response::Success {
            name: String::from("TEST"),
            is_extension: true,
            arguments: DelimitedArguments {
                delimiter: Some(String::from(":")),
                arguments: Arguments::ParenthesisDelimitedArgumentLists(vec![
                    vec![Argument::PrimitiveArgument(String::from("1"))],
                    vec![
                        Argument::PrimitiveArgument(String::from("2")),
                        Argument::PrimitiveArgument(String::from("abc")),
                    ],
                ]),
                terminator: None,
            },
        },
    )
}

// Paren delimited multiple argument lists with key-value elements
#[test]
fn multiple_paren_kv_args() {
    test_parse(
        "+TEST: (1=abc)(2,xyz=3)",
        Response::Success {
            name: String::from("TEST"),
            is_extension: true,
            arguments: DelimitedArguments {
                delimiter: Some(String::from(":")),
                arguments: Arguments::ParenthesisDelimitedArgumentLists(vec![
                    vec![Argument::KeyValueArgument {
                        key: String::from("1"),
                        value: String::from("abc"),
                    }],
                    vec![
                        Argument::PrimitiveArgument(String::from("2")),
                        Argument::KeyValueArgument {
                            key: String::from("xyz"),
                            value: String::from("3"),
                        },
                    ],
                ]),
                terminator: None,
            },
        },
    )
}
