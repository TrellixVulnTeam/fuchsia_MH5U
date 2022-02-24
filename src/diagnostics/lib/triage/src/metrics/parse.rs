// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::metrics::{
        internal_bug, variable::VariableName, ExpressionTree, Function, MathFunction, MetricValue,
    },
    anyhow::{format_err, Error},
    nom::{
        branch::alt,
        bytes::complete::{tag, take_until, take_while, take_while_m_n},
        character::{complete::char, is_alphabetic, is_alphanumeric},
        combinator::{all_consuming, map, recognize},
        error::{convert_error, VerboseError},
        multi::separated_list,
        number::complete::double,
        sequence::{delimited, pair, preceded, separated_pair, terminated, tuple},
        Err::{self, Incomplete},
        IResult,
    },
};

// The 'nom' crate supports buiding parsers by combining functions into more
// powerful functions. Combined functions can be applied to a sequence of
// chars (or bytes) and will parse into the sequence as far as possible (left
// to right) returning the result of the parse and the remainder of the sequence.
//
// This parser parses infix math expressions with operators
// + - * / > < >= <= == () [] following standard order of operations.
// It also supports functions like FuncName(expr, expr, expr...)
// () must contain one expression, except when it's part of a function.
// [] contains a comma-separated list of expressions.
//
// Combinators (parse-function builders) used in this parser:
// alt: Allows backtracking and trying an alternative parse.
// tag: Matches and returns a fixed string.
// take_while: Matches and returns characters as long as they satisfy a condition.
// take_while_m_n: Take_while constrained to return at least M and at most N chars.
// char: Matches and returns a single character.
// all_consuming: The parser must use all characters.
// map: Applies a transformation function to the return type of a parser.
// double: Parses an f64 and returns its value.
// delimited: Applies three parsers and returns the result of the middle one.
// preceded: Applies two parsers and returns the result of the second one.
// terminated: Applies two parsers and returns the result of the first one.
// separated_list: Takes two parsers, a separator and element, and returns a Vec of elements.
// separated_pair: Applies three parsers and returns a tuple of the first and third results.
// tuple: Takes a tuple of parsers and returns a tuple of the corresponding results.
//
//  In addition, two boolean functions match characters:
// is_alphabetic: ASCII a..zA..Z
// is_alphanumeric: ASCII a..zA..Z0..9
//
// VerboseError stores human-friendly information about parse errors.
// convert_error() produces a human-friendly string from a VerboseError.
//
// This parser accepts whitespace. For consistency, whitespace is accepted
//  _before_ the non-whitespace that the parser is trying to match.

// Matches 0 or more whitespace characters: \n, \t, ' '.
fn whitespace<'a>(i: &'a str) -> IResult<&'a str, &'a str, VerboseError<&'a str>> {
    take_while(move |c| " \n\t".contains(c))(i)
}

// spewing() is useful for debugging. If you touch this file, you will
// likely want to uncomment and use it. Wrap any parser in
// spewing("diagnostic string", parser) to get lots of printouts showing
// how far the parser has gotten and what strings it's seeing.
// Remember that every backtrack (alt) will produce lots of output.

/*use std::cmp::min;
fn spewing<'a, F, O>(
    note: &'static str,
    parser: F,
) -> impl Fn(&'a str) -> IResult<&'a str, O, VerboseError<&'a str>>
where
    F: Fn(&'a str) -> IResult<&'a str, O, VerboseError<&'a str>>,
{
    let dumper = move |i: &'a str| {
        println!("{}:'{}'", note, &i[..min(20, i.len())]);
        Ok((i, ()))
    };
    preceded(dumper, parser)
}*/

// A bit of syntactic sugar - just adds optional whitespace in front of any parser.
fn spaced<'a, F, O>(parser: F) -> impl Fn(&'a str) -> IResult<&'a str, O, VerboseError<&'a str>>
where
    F: Fn(&'a str) -> IResult<&'a str, O, VerboseError<&'a str>>,
{
    preceded(whitespace, parser)
}

// Parses a name with the first character alphabetic or '_' and 0..n additional
// characters alphanumeric or '_'.
fn simple_name<'a>(i: &'a str) -> IResult<&'a str, &'a str, VerboseError<&'a str>> {
    recognize(pair(
        take_while_m_n(1, 1, |c: char| c.is_ascii() && (is_alphabetic(c as u8) || c == '_')),
        take_while(|c: char| c.is_ascii() && (is_alphanumeric(c as u8) || c == '_')),
    ))(i)
}

// Parses two simple names joined by "::" to form a namespaced name. Returns a
// Metric-type Expression holding the namespaced name.
fn name_with_namespace<'a>(i: &'a str) -> IResult<&'a str, ExpressionTree, VerboseError<&'a str>> {
    map(separated_pair(simple_name, tag("::"), simple_name), move |(s1, s2)| {
        ExpressionTree::Variable(VariableName::new(format!("{}::{}", s1, s2)))
    })(i)
}

// Parses a simple name with no namespace and returns a Metric-type Expression
// holding the simple name.
fn name_no_namespace<'a>(i: &'a str) -> IResult<&'a str, ExpressionTree, VerboseError<&'a str>> {
    map(simple_name, move |s: &str| ExpressionTree::Variable(VariableName::new(s.to_string())))(i)
}

// Parses either a simple or namespaced name and returns a Metric-type Expression
// holding it.
fn name<'a>(i: &'a str) -> IResult<&'a str, ExpressionTree, VerboseError<&'a str>> {
    alt((name_with_namespace, name_no_namespace))(i)
}

// Returns a Value-type expression holding either an Int or Float number.
//
// Every int can be parsed as a float. The float parser is applied first. If
// it finds a number, number() attempts to parse those same characters as an int.
// If it succeeds, it treats the number as an Int type.
// Note that this handles unary + and -.
fn number<'a>(i: &'a str) -> IResult<&'a str, ExpressionTree, VerboseError<&'a str>> {
    match double(i) {
        Ok((remaining, float)) => {
            let number_len = i.len() - remaining.len(); // How many characters were accepted
            match i[..number_len].parse::<i64>() {
                Ok(int) => Ok((&i[number_len..], ExpressionTree::Value(MetricValue::Int(int)))),
                Err(_) => Ok((&i[number_len..], ExpressionTree::Value(MetricValue::Float(float)))),
            }
        }
        Err(error) => return Err(error),
    }
}

macro_rules! any_string {
    ($left:expr, $mid:expr, $right:expr, $i:expr) => {
        match delimited($left, $mid, $right)($i) {
            Ok((remaining, text)) => {
                let next_pos = $i.len() - remaining.len();
                Ok((&$i[next_pos..], ExpressionTree::Value(MetricValue::String(text.to_string()))))
            }
            Err(e) => Err(e),
        }
    };
}

fn single_quote_string<'a>(i: &'a str) -> IResult<&'a str, ExpressionTree, VerboseError<&'a str>> {
    any_string!(char('\''), take_until("'"), char('\''), i)
}

fn escaped_single_quote_string<'a>(
    i: &'a str,
) -> IResult<&'a str, ExpressionTree, VerboseError<&'a str>> {
    any_string!(tag("\'"), take_until("\'"), tag("\'"), i)
}

fn double_quote_string<'a>(i: &'a str) -> IResult<&'a str, ExpressionTree, VerboseError<&'a str>> {
    any_string!(char('\"'), take_until("\""), char('\"'), i)
}

fn escaped_double_quote_string<'a>(
    i: &'a str,
) -> IResult<&'a str, ExpressionTree, VerboseError<&'a str>> {
    any_string!(tag("\""), take_until("\""), tag("\""), i)
}

// Returns a Value-type expression holding a String.
//
// Will match the following strings
// - "'hello'"
// - '"hello"'
// - "\"hello\""
// - '\'hello\''
fn string<'a>(i: &'a str) -> IResult<&'a str, ExpressionTree, VerboseError<&'a str>> {
    alt((
        single_quote_string,
        escaped_single_quote_string,
        double_quote_string,
        escaped_double_quote_string,
    ))(i)
}

macro_rules! function {
    ($tag:expr, $function:ident) => {
        (map(spaced(tag($tag)), move |_| Function::$function))
    };
}

macro_rules! math {
    ($tag:expr, $function:ident) => {
        (map(spaced(tag($tag)), move |_| Function::Math(MathFunction::$function)))
    };
}

fn function_name_parser<'a>(i: &'a str) -> IResult<&'a str, Function, VerboseError<&'a str>> {
    // alt has a limited number of args, so must be nested.
    // At some point, worry about efficiency.
    // Make sure that if one function is a prefix of another, the longer one comes first or the
    // shorter one will match and short-circuit.
    alt((
        alt((
            function!("And", And),
            function!("Or", Or),
            function!("Not", Not),
            math!("Max", Max),
            function!("Minutes", Minutes), // Parser must try "Minutes" before "Min"
            math!("Min", Min),
            function!("SyslogHas", SyslogHas),
            function!("KlogHas", KlogHas),
            function!("BootlogHas", BootlogHas),
            function!("Missing", Missing),
            function!("UnhandledType", UnhandledType),
            function!("Problem", Problem),
            function!("Annotation", Annotation),
        )),
        alt((
            function!("Fn", Lambda),
            function!("Map", Map),
            function!("Fold", Fold),
            function!("Filter", Filter),
            function!("Apply", Apply),
            function!("Count", Count),
            function!("Nanos", Nanos),
            function!("Micros", Micros),
            function!("Millis", Millis),
            function!("Seconds", Seconds),
            function!("Hours", Hours),
            function!("Days", Days),
            function!("Now", Now),
            function!("Option", OptionF),
            function!("StringMatches", StringMatches),
        )),
    ))(i)
}

fn function_expression<'a>(i: &'a str) -> IResult<&'a str, ExpressionTree, VerboseError<&'a str>> {
    let open_paren = spaced(char('('));
    let expressions = separated_list(spaced(char(',')), expression_top);
    let close_paren = spaced(char(')'));
    let function_sequence = tuple((function_name_parser, open_paren, expressions, close_paren));
    map(function_sequence, move |(function, _, operands, _)| {
        ExpressionTree::Function(function, operands)
    })(i)
}

fn vector_expression<'a>(i: &'a str) -> IResult<&'a str, ExpressionTree, VerboseError<&'a str>> {
    let open_bracket = spaced(char('['));
    let expressions = separated_list(spaced(char(',')), expression_top);
    let close_bracket = spaced(char(']'));
    let vector_sequence = tuple((open_bracket, expressions, close_bracket));
    map(vector_sequence, move |(_, items, _)| ExpressionTree::Vector(items))(i)
}

// I use "primitive" to mean an expression that is not an infix operator pair:
// a primitive value, a metric name, a function (simple name followed by
// parenthesized expression list), or any expression contained by ( ) or [ ].
fn expression_primitive<'a>(i: &'a str) -> IResult<&'a str, ExpressionTree, VerboseError<&'a str>> {
    let paren_expr = delimited(char('('), terminated(expression_top, whitespace), char(')'));
    let res = spaced(alt((
        paren_expr,
        function_expression,
        vector_expression,
        name,
        alt((number, string)),
    )))(i);
    res
}

// It's hard to parse a list of operator-separated expressions correctly,
// given that they have to be evaluated from left to right.
//
// Recursive parse is tempting, something like:
//   addsub = (addsub +/- muldiv) | muldiv
// In a left-to-right-parsing combinator, the above causes infinite recursion.
//
//   addsub = (muldiv +/- addsub) | muldiv
// This causes the expression tree to be built right-to-left.
//
// So what about an iterative approach?
// nom's separated_list() doesn't return the separators, so it can't be used
// when we care about the separators - as we do, for example, in a sequence
// of expressions separated by either '+' or '-'.
//
// In the end, I rolled my own iterative parser that first recognizes
//   item (separator item)*
// and then builds the Expression tree.
//
// TODO(cphoenix): See whether it's cleaner to build the tree during the
// recognize step (given that the Expression-type has to be set based on
// the separator that's found)

// This function produces lists of items and separators. Note that it takes
// the input as a parameter and returns the parsed information (or error)
// directly - it is not a combinator function. It must find at least one item,
// or return Error.
fn items_and_separators<'a, F, G, O1, O2>(
    item_parser: F,
    separator_parser: G,
    i: &'a str,
) -> IResult<&'a str, (std::vec::Vec<O1>, std::vec::Vec<O2>), VerboseError<&'a str>>
where
    F: Fn(&'a str) -> IResult<&'a str, O1, VerboseError<&'a str>>,
    G: Fn(&'a str) -> IResult<&'a str, O2, VerboseError<&'a str>>,
{
    let mut items = Vec::new();
    let mut operators = Vec::new();
    let mut remainder = i;
    {
        // Fasten your seatbelts. Inside this block, we start by wrapping
        // the given item_parser and separator_parser. The wrapped versions
        // have side effects!
        //
        // When the inner parsers match, the wrapper adds their output to the
        // appropriate vec. This does not allow backtracking, but that's OK.
        // The whole items-and-separators succeeds or fails together; another
        // way to say this is that every valid separator must be followed by
        // a valid item, or the whole parse attempt is invalid and
        // items_and_separators() will return Error.
        //
        // The wrappers borrow the vec's mutably, so they're inside a block to
        // drop them when their work is done.
        let mut item_parse = |i| match spaced(&item_parser)(i) {
            Err(err) => Err(err),
            Ok((r, exp)) => {
                items.push(exp);
                Ok((r, ()))
            }
        };
        let mut separator_parse = |i| match preceded::<_, _, _, VerboseError<&'a str>, _, _>(
            whitespace,
            &separator_parser,
        )(i)
        {
            Err(err) => Err(err),
            Ok((r, exp)) => {
                operators.push(exp);
                Ok((r, ()))
            }
        };
        // Now that the wrapped parsers are defined, we can match the first item,
        // then loop looking for separators and additional items.
        match item_parse(remainder) {
            Err(e) => return Err(e),     // We must find at least one item.
            Ok((r, _)) => remainder = r, // The parsed item is now in items.
        }
        loop {
            match separator_parse(remainder) {
                Err(_) => break, // Ending on an item, not finding a separator, is fine.
                // Note how the remainder of each parse is fed into the next parse.
                Ok((r, _)) => match item_parse(r) {
                    // If we find a separator...
                    Err(e) => return Err(e), // We'd better find an item after it.
                    Ok((r, _)) => remainder = r,
                },
            }
        }
    }
    // The parser-wrap block is closed, and we can use items and operators again.
    // Build a happy-combinator-return type, a tuple with the first member being
    // the remaining unparsed characters, and the second item being the result -
    // in this case, a tuple containing the vec's of items and operators.
    Ok((remainder, (items, operators)))
}

// This takes the lists of items and operators produced by items_and_separators()
// and builds an Expression.
fn build_expression<'a>(
    mut items: Vec<ExpressionTree>,
    mut operators: Vec<Function>,
) -> ExpressionTree {
    // We want to evaluate the leftmost operator first, which means it has to be
    // lowest in the tree. The leftmost was parsed first, so it's lowest in the
    // vec's. Popping is more efficient than deleting item 0 and shifting, so
    // reverse the vec's before we start.
    items.reverse();
    operators.reverse();
    let mut res =
        items.pop().unwrap_or(ExpressionTree::Value(internal_bug("Bug in parser: zero items")));
    for _i in 0..operators.len() {
        let args = vec![
            res,
            items
                .pop()
                .unwrap_or(ExpressionTree::Value(internal_bug("Bug in parser: too few items"))),
        ];
        res = ExpressionTree::Function(operators.pop().unwrap(), args);
    }
    res
}

// Scans for primitive expressions separated by * and /.
fn expression_muldiv<'a>(i: &'a str) -> IResult<&'a str, ExpressionTree, VerboseError<&'a str>> {
    let (remainder, (items, operators)) = items_and_separators(
        expression_primitive,
        alt((math!("*", Mul), math!("//", IntDiv), math!("/", FloatDiv))),
        i,
    )?;
    Ok((remainder, build_expression(items, operators)))
}

// Scans for muldiv expressions (which may be a single primitive expression)
// separated by + and -. Remember unary + and - will be recognized by number().
fn expression_addsub<'a>(i: &'a str) -> IResult<&'a str, ExpressionTree, VerboseError<&'a str>> {
    let (remainder, (items, operators)) =
        items_and_separators(expression_muldiv, alt((math!("+", Add), math!("-", Sub))), i)?;
    Ok((remainder, build_expression(items, operators)))
}

// Top-level expression. Should match the entire expression string, and also
// can be used inside parentheses.
fn expression_top<'a>(i: &'a str) -> IResult<&'a str, ExpressionTree, VerboseError<&'a str>> {
    // Note: alt() is not BNF - it's sequential. It's important to put the longer strings first.
    // If a shorter substring succeeds where it shouldn't, the alt() may not get another chance.
    let comparison = alt((
        math!(">=", GreaterEq),
        math!("<=", LessEq),
        function!("==", Equals),
        function!("!=", NotEq),
        math!(">", Greater),
        math!("<", Less),
    ));
    alt((
        map(tuple((expression_addsub, comparison, expression_addsub)), move |(left, op, right)| {
            ExpressionTree::Function(op, vec![left, right])
        }),
        expression_addsub,
    ))(i)
}

// Parses a given string into either an Error or an Expression ready
// to be evaluated.
pub(crate) fn parse_expression(i: &str) -> Result<ExpressionTree, Error> {
    let match_whole = all_consuming(terminated(expression_top, whitespace));
    match match_whole(i) {
        Err(Err::Error(e)) | Err(Err::Failure(e)) => {
            return Err(format_err!("Expression Error: \n{}", convert_error(i, e)))
        }
        Ok((_, result)) => Ok(result),
        Err(Incomplete(what)) => {
            return Err(format_err!("Why did I get an incomplete? {:?}", what))
        }
    }
}

#[cfg(test)]
mod test {
    use {
        super::*,
        crate::{
            assert_problem,
            metrics::{ExpressionTree, Fetcher, MathFunction, MetricState, TrialDataFetcher},
        },
        std::collections::HashMap,
    };

    // Res, simplify_fn, and get_parse are necessary because IResult can't be compared and can't
    //   easily be matched/decomposed. Res can be compared and debug-formatted.
    // Call get_parse(parse_function, string) to get either Ok(remainder_str, result)
    //   or Err(descriptive_string).
    #[derive(PartialEq, Debug)]
    enum Res<'a, T> {
        Ok(&'a str, T),
        Err(String),
    }

    fn simplify_fn<'a, T: std::fmt::Debug>(
        i: &str,
        r: IResult<&'a str, T, VerboseError<&'a str>>,
    ) -> Res<'a, T> {
        match r {
            Err(Err::Error(e)) => Res::Err(format!("Error: \n{:?}", convert_error(i, e))),
            Err(Err::Failure(e)) => Res::Err(format!("Failure: \n{:?}", convert_error(i, e))),
            Err(Incomplete(e)) => Res::Err(format!("Incomplete: {:?}", e)),
            Ok((unused, result)) => Res::Ok(unused, result),
        }
    }

    macro_rules! get_parse {
        ($fn:expr, $string:expr) => {
            simplify_fn($string, $fn($string))
        };
    }

    impl<'a, T> Res<'a, T> {
        fn is_err(&self) -> bool {
            match self {
                Res::Err(_) => true,
                Res::Ok(_, _) => false,
            }
        }
    }

    #[fuchsia::test]
    fn parse_vectors() {
        fn v(i: i64) -> ExpressionTree {
            ExpressionTree::Value(MetricValue::Int(i))
        }

        assert_eq!(
            get_parse!(expression_primitive, "[1,2]"),
            Res::Ok("", ExpressionTree::Vector(vec![v(1), v(2)]))
        );
        assert_eq!(
            get_parse!(expression_primitive, " [ 1 , 2 ] "),
            Res::Ok(" ", ExpressionTree::Vector(vec![v(1), v(2)]))
        );
        assert_eq!(
            get_parse!(expression_primitive, "[1]"),
            Res::Ok("", ExpressionTree::Vector(vec![v(1)]))
        );
        assert_eq!(
            get_parse!(expression_primitive, "[]"),
            Res::Ok("", ExpressionTree::Vector(Vec::new()))
        );
        let first = ExpressionTree::Function(Function::Math(MathFunction::Add), vec![v(1), v(2)]);
        let second = ExpressionTree::Function(Function::Math(MathFunction::Sub), vec![v(2), v(1)]);
        assert_eq!(
            get_parse!(expression_primitive, "[1+2, 2-1]"),
            Res::Ok("", ExpressionTree::Vector(vec![first, second]))
        );
        // Verify that we reject un-closed braces.
        assert!(get_parse!(expression_primitive, "[1+2, 2-1").is_err());
        // Verify that it's just the unclosed brace that was the problem in the previous line.
        // (The parser will only look for the first complete primitive expression.)
        assert_eq!(get_parse!(expression_primitive, "1+2, 2-1"), Res::Ok("+2, 2-1", v(1)));
        assert!(get_parse!(expression_primitive, "]").is_err());
        assert!(get_parse!(expression_primitive, "[").is_err());
    }

    #[fuchsia::test]
    fn parse_numbers() {
        // No leading extraneous characters allowed in number, not even whitespace.
        assert!(get_parse!(number, "f5").is_err());
        assert!(get_parse!(number, " 1").is_err());
        // Empty string should fail
        assert!(get_parse!(number, "").is_err());
        // Trailing characters will be returned as unused remainder
        assert_eq!(
            get_parse!(number, "1 "),
            Res::Ok(" ", ExpressionTree::Value(MetricValue::Int(1)))
        );
        assert_eq!(
            get_parse!(number, "1a"),
            Res::Ok("a", ExpressionTree::Value(MetricValue::Int(1)))
        );
        // If it parses as int, it's an int.
        assert_eq!(
            get_parse!(number, "234"),
            Res::Ok("", ExpressionTree::Value(MetricValue::Int(234)))
        );
        // Otherwise it's a float.
        assert_eq!(
            get_parse!(number, "234.0"),
            Res::Ok("", ExpressionTree::Value(MetricValue::Float(234.0)))
        );
        assert_eq!(
            get_parse!(number, "234.0e-5"),
            Res::Ok("", ExpressionTree::Value(MetricValue::Float(234.0e-5)))
        );
        // Leading -, +, 0 are all OK for int
        assert_eq!(
            get_parse!(number, "0"),
            Res::Ok("", ExpressionTree::Value(MetricValue::Int(0)))
        );
        assert_eq!(
            get_parse!(number, "00234"),
            Res::Ok("", ExpressionTree::Value(MetricValue::Int(234)))
        );
        assert_eq!(
            get_parse!(number, "+234"),
            Res::Ok("", ExpressionTree::Value(MetricValue::Int(234)))
        );
        assert_eq!(
            get_parse!(number, "-234"),
            Res::Ok("", ExpressionTree::Value(MetricValue::Int(-234)))
        );
        // Leading +, -, 0 are OK for float.
        assert_eq!(
            get_parse!(number, "0.0"),
            Res::Ok("", ExpressionTree::Value(MetricValue::Float(0.0)))
        );
        assert_eq!(
            get_parse!(number, "00234.0"),
            Res::Ok("", ExpressionTree::Value(MetricValue::Float(234.0)))
        );
        assert_eq!(
            get_parse!(number, "+234.0"),
            Res::Ok("", ExpressionTree::Value(MetricValue::Float(234.0)))
        );
        assert_eq!(
            get_parse!(number, "-234.0"),
            Res::Ok("", ExpressionTree::Value(MetricValue::Float(-234.0)))
        );
        // Leading and trailing periods parse as valid float.
        assert_eq!(
            get_parse!(number, ".1"),
            Res::Ok("", ExpressionTree::Value(MetricValue::Float(0.1)))
        );
        assert_eq!(
            get_parse!(number, "1."),
            Res::Ok("", ExpressionTree::Value(MetricValue::Float(1.0)))
        );
        assert_eq!(
            get_parse!(number, "1.a"),
            Res::Ok("a", ExpressionTree::Value(MetricValue::Float(1.0)))
        );
        // "e" must be followed by a number
        assert!(get_parse!(number, "1.e").is_err());
    }

    #[fuchsia::test]
    fn parse_string() {
        // needs to have quotes
        assert!(get_parse!(string, "OK").is_err());

        // needs to close its quotes
        assert!(get_parse!(string, "'OK").is_err());

        assert_eq!(
            get_parse!(string, "'OK'"),
            Res::Ok("", ExpressionTree::Value(MetricValue::String("OK".to_string())))
        );
        assert_eq!(
            get_parse!(string, "'OK'a"),
            Res::Ok("a", ExpressionTree::Value(MetricValue::String("OK".to_string())))
        );
        assert_eq!(
            get_parse!(string, r#""OK""#),
            Res::Ok("", ExpressionTree::Value(MetricValue::String("OK".to_string())))
        );
        assert_eq!(
            get_parse!(string, "\'OK\'"),
            Res::Ok("", ExpressionTree::Value(MetricValue::String("OK".to_string())))
        );
        assert_eq!(
            get_parse!(string, "\"OK\""),
            Res::Ok("", ExpressionTree::Value(MetricValue::String("OK".to_string())))
        );

        // can handle nested qoutes
        assert_eq!(
            get_parse!(string, r#"'a"b'"#),
            Res::Ok("", ExpressionTree::Value(MetricValue::String(r#"a"b"#.to_string())))
        );
        assert_eq!(
            get_parse!(string, r#""a'b""#),
            Res::Ok("", ExpressionTree::Value(MetricValue::String("a'b".to_string())))
        );

        // can handle whitespace
        assert_eq!(
            get_parse!(string, "'OK OK'"),
            Res::Ok("", ExpressionTree::Value(MetricValue::String("OK OK".to_string())))
        );

        // can parse strings that are numbers
        assert_eq!(
            get_parse!(string, "'123'"),
            Res::Ok("", ExpressionTree::Value(MetricValue::String("123".to_string())))
        );
    }

    macro_rules! variable_expression {
        ($name:expr) => {
            ExpressionTree::Variable(VariableName::new($name.to_owned()))
        };
    }

    #[fuchsia::test]
    fn parse_names_no_namespace() {
        assert_eq!(get_parse!(name_no_namespace, "abc"), Res::Ok("", variable_expression!("abc")));
        assert_eq!(get_parse!(name_no_namespace, "bc."), Res::Ok(".", variable_expression!("bc")));
        // Names can contain digits and _ but can't start with digits
        assert_eq!(
            get_parse!(name_no_namespace, "bc42."),
            Res::Ok(".", variable_expression!("bc42"))
        );
        assert!(get_parse!(name_no_namespace, "42bc.").is_err());
        assert_eq!(
            get_parse!(name_no_namespace, "_bc42_"),
            Res::Ok("", variable_expression!("_bc42_"))
        );
        assert_eq!(
            get_parse!(name_no_namespace, "_bc42_::abc"),
            Res::Ok("::abc", variable_expression!("_bc42_"))
        );
        assert_eq!(
            get_parse!(name_no_namespace, "_bc42_:abc"),
            Res::Ok(":abc", variable_expression!("_bc42_"))
        );
    }

    #[fuchsia::test]
    fn parse_names_with_namespace() {
        assert_eq!(
            get_parse!(name_with_namespace, "_bc42_::abc"),
            Res::Ok("", variable_expression!("_bc42_::abc"))
        );
        assert_eq!(
            get_parse!(name_with_namespace, "_bc42_::abc::def"),
            Res::Ok("::def", variable_expression!("_bc42_::abc"))
        );
        assert!(get_parse!(name_with_namespace, "_bc42_:abc::def").is_err());
    }

    #[fuchsia::test]
    fn parse_names() {
        assert_eq!(
            get_parse!(name, "_bc42_::abc"),
            Res::Ok("", variable_expression!("_bc42_::abc"))
        );
        assert_eq!(
            get_parse!(name, "_bc42_:abc::def"),
            Res::Ok(":abc::def", variable_expression!("_bc42_"))
        );
        assert_eq!(
            get_parse!(name, "_bc42_::abc::def"),
            Res::Ok("::def", variable_expression!("_bc42_::abc"))
        );
    }

    macro_rules! eval {
        ($e:expr) => {
            MetricState::evaluate_math($e)
        };
    }

    #[fuchsia::test]
    fn parse_number_types() -> Result<(), Error> {
        assert_eq!(eval!("2"), MetricValue::Int(2));
        assert_eq!(eval!("2+3"), MetricValue::Int(5));
        assert_eq!(eval!("2.0+3"), MetricValue::Float(5.0));
        assert_eq!(eval!("2+3.0"), MetricValue::Float(5.0));
        assert_eq!(eval!("2.0+2.0"), MetricValue::Float(4.0));

        Ok(())
    }

    #[fuchsia::test]
    fn parse_div_operations() -> Result<(), Error> {
        assert_eq!(eval!("5.0/2"), MetricValue::Float(2.5));
        assert_eq!(eval!("-5.0/2"), MetricValue::Float(-2.5));
        assert_eq!(eval!("5.0/2.0"), MetricValue::Float(2.5));
        assert_eq!(eval!("-5.0/2.0"), MetricValue::Float(-2.5));
        assert_eq!(eval!("5/2"), MetricValue::Float(2.5));
        assert_eq!(eval!("-5/2"), MetricValue::Float(-2.5));

        // int division should truncate towards zero
        assert_eq!(eval!("5.0//2"), MetricValue::Float(2.0));
        assert_eq!(eval!("-5.0//2"), MetricValue::Float(-2.0));
        assert_eq!(eval!("5.0//2.0"), MetricValue::Float(2.0));
        assert_eq!(eval!("-5.0//2.0"), MetricValue::Float(-2.0));
        assert_eq!(eval!("5//2"), MetricValue::Int(2));
        assert_eq!(eval!("-5//2"), MetricValue::Int(-2));

        // test truncation happens after division
        assert_eq!(eval!("5000//5.1"), MetricValue::Float(980.0));
        Ok(())
    }

    #[fuchsia::test]
    fn parse_operator_precedence() -> Result<(), Error> {
        assert_eq!(eval!("2+3*4"), MetricValue::Int(14));
        assert_eq!(eval!("2+3*4>14-1*1"), MetricValue::Bool(true));
        assert_eq!(eval!("3*4+2"), MetricValue::Int(14));
        assert_eq!(eval!("2-3-4"), MetricValue::Int(-5));
        assert_eq!(eval!("6//3*4"), MetricValue::Int(8));
        assert_eq!(eval!("2-3-4"), MetricValue::Int(-5));
        assert_eq!(eval!("(2+3)*4"), MetricValue::Int(20));
        assert_eq!(eval!("2++4"), MetricValue::Int(6));
        assert_eq!(eval!("2+-4"), MetricValue::Int(-2));
        assert_eq!(eval!("2-+4"), MetricValue::Int(-2));
        assert_eq!(eval!("2--4"), MetricValue::Int(6));
        Ok(())
    }

    #[fuchsia::test]
    fn parser_accepts_whitespace() -> Result<(), Error> {
        assert_eq!(eval!(" 2 + +3 * 4 - 5 // ( -2 + Min ( -2 , 3 ) ) "), MetricValue::Int(15));
        Ok(())
    }

    #[fuchsia::test]
    fn parser_comparisons() -> Result<(), Error> {
        assert_eq!(
            format!("{:?}", parse_expression("2>1")),
            "Ok(Function(Math(Greater), [Value(Int(2)), Value(Int(1))]))"
        );
        assert_eq!(eval!("2>2"), MetricValue::Bool(false));
        assert_eq!(eval!("2>=2"), MetricValue::Bool(true));
        assert_eq!(eval!("2<2"), MetricValue::Bool(false));
        assert_eq!(eval!("2<=2"), MetricValue::Bool(true));
        assert_eq!(eval!("2==2"), MetricValue::Bool(true));
        assert_eq!(eval!("2==2.0"), MetricValue::Bool(true));

        // can do string comparison
        assert_eq!(eval!("'a'=='a'"), MetricValue::Bool(true));
        assert_eq!(eval!("'a'!='a'"), MetricValue::Bool(false));
        assert_eq!(eval!("'a'!='b'"), MetricValue::Bool(true));

        // check variants of string parsing
        assert_eq!(eval!(r#""a"=="a""#), MetricValue::Bool(true));
        assert_eq!(eval!(r#"'a'=="a""#), MetricValue::Bool(true));

        // There can be only one.
        assert!(parse_expression("2==2==2").is_err());
        Ok(())
    }

    #[fuchsia::test]
    fn parser_boolean_functions_value() -> Result<(), Error> {
        assert_eq!(
            format!("{:?}", parse_expression("Not(2>1)")),
            "Ok(Function(Not, [Function(Math(Greater), [Value(Int(2)), Value(Int(1))])]))"
        );
        assert_eq!(eval!("And(2>1, 2>2)"), MetricValue::Bool(false));
        assert_eq!(eval!("And(2>2, 2>1)"), MetricValue::Bool(false));
        assert_eq!(eval!("And(2>2, 2>2)"), MetricValue::Bool(false));
        assert_eq!(eval!("And(2>1, 2>1)"), MetricValue::Bool(true));
        assert_eq!(eval!("Or(2>1, 2>2)"), MetricValue::Bool(true));
        assert_eq!(eval!("Or(2>2, 2>1)"), MetricValue::Bool(true));
        assert_eq!(eval!("Or(2>2, 2>2)"), MetricValue::Bool(false));
        assert_eq!(eval!("Or(2>1, 2>1)"), MetricValue::Bool(true));
        assert_eq!(eval!("Not(2>1)"), MetricValue::Bool(false));
        assert_eq!(eval!("Not(2>2)"), MetricValue::Bool(true));
        Ok(())
    }

    #[fuchsia::test]
    fn parser_boolean_functions_args() -> Result<(), Error> {
        assert_eq!(eval!("And(2>1)"), MetricValue::Bool(true));
        assert_eq!(eval!("And(2>1, 2>1, 2>1)"), MetricValue::Bool(true));
        assert_problem!(eval!("And()"), "SyntaxError: No operands in boolean expression");
        assert_eq!(eval!("Or(2>1)"), MetricValue::Bool(true));
        assert_eq!(eval!("Or(2>1, 2>1, 2>1)"), MetricValue::Bool(true));
        assert_problem!(eval!("Or()"), "SyntaxError: No operands in boolean expression");
        assert_problem!(
            eval!("Not(2>1, 2>1)"),
            "SyntaxError: Wrong number of arguments (2) for unary bool operator"
        );
        assert_problem!(
            eval!("Not()"),
            "SyntaxError: Wrong number of arguments (0) for unary bool operator"
        );
        Ok(())
    }

    #[fuchsia::test]
    fn parser_maxmin_functions() -> Result<(), Error> {
        assert_eq!(eval!("Max(2, 5, 3, -1)"), MetricValue::Int(5));
        assert_eq!(eval!("Min(2, 5, 3, -1)"), MetricValue::Int(-1));
        assert_eq!(eval!("Min(2)"), MetricValue::Int(2));
        assert_eq!(eval!("Max(2)"), MetricValue::Int(2));
        assert_problem!(eval!("Max()"), "SyntaxError: No operands in math expression");
        assert_problem!(eval!("Min()"), "SyntaxError: No operands in math expression");
        Ok(())
    }

    #[fuchsia::test]
    fn parser_time_functions() -> Result<(), Error> {
        assert_eq!(eval!("Nanos(5)"), MetricValue::Int(5));
        assert_eq!(eval!("Micros(4)"), MetricValue::Int(4_000));
        assert_eq!(eval!("Millis(5)"), MetricValue::Int(5_000_000));
        assert_eq!(eval!("Seconds(2)"), MetricValue::Int(2_000_000_000));
        assert_eq!(eval!("Minutes(2)"), MetricValue::Int(2_000_000_000 * 60));
        assert_eq!(eval!("Hours(2)"), MetricValue::Int(2_000_000_000 * 60 * 60));
        assert_eq!(eval!("Days(2)"), MetricValue::Int(2_000_000_000 * 60 * 60 * 24));
        // Floating point values work.
        assert_eq!(eval!("Seconds(0.5)"), MetricValue::Int(500_000_000));
        // Negative values are fine.
        assert_eq!(eval!("Seconds(-0.5)"), MetricValue::Int(-500_000_000));
        // Non-numeric or bad arg combinations return Problem.
        assert_problem!(eval!("Hours()"), "SyntaxError: Time conversion needs 1 numeric argument");
        assert_problem!(
            eval!("Hours(2, 3)"),
            "SyntaxError: Time conversion needs 1 numeric argument"
        );
        assert_problem!(
            eval!("Hours('a')"),
            "ValueError: Time conversion needs 1 numeric argument, not String(a)"
        );
        assert_problem!(eval!("1.0/0.0"), "ValueError: Division by zero");
        assert_problem!(eval!("Hours(1.0/0.0)"), "ValueError: Division by zero");
        Ok(())
    }

    #[fuchsia::test]
    fn parser_nested_function() -> Result<(), Error> {
        assert_eq!(eval!("Max(2, Min(4-1, 5))"), MetricValue::Int(3));
        assert_eq!(eval!("And(Max(1, 2+3)>1, Or(1>2, 2>1))"), MetricValue::Bool(true));
        Ok(())
    }

    #[fuchsia::test]
    fn singleton_vecs_promote() -> Result<(), Error> {
        assert_eq!(eval!("Max([1+1], Min([4]-1, 4+[1]))"), MetricValue::Int(3));
        assert_eq!(eval!("And(Max(1, 2+[3])>1, Or([1]>2, [1>2], 2>[1]))"), MetricValue::Bool(true));
        Ok(())
    }

    fn i(i: i64) -> MetricValue {
        MetricValue::Int(i)
    }
    fn v(v: &[MetricValue]) -> MetricValue {
        MetricValue::Vector(v.to_vec())
    }

    #[fuchsia::test]
    fn functional_programming() -> Result<(), Error> {
        assert_eq!(eval!("Apply(Fn([], 5), [])"), i(5));
        assert_eq!(eval!("Apply(Fn([a], a+5), [2])"), i(7));
        assert_eq!(eval!("Apply(Fn([a, b], a*b+5), [2, 3])"), i(11));
        assert_eq!(eval!("Map(Fn([a], a*2), [1,2,3])"), v(&[i(2), i(4), i(6)]));
        assert_eq!(
            eval!("Map(Fn([a, b], [a, b]), [1, 2, 3], [4, 5, 6])"),
            v(&[v(&[i(1), i(4)]), v(&[i(2), i(5)]), v(&[i(3), i(6)])])
        );
        assert_eq!(eval!("Map(Fn([a, b], [a, b]), [1, 2, 3], [4])"), v(&[v(&[i(1), i(4)])]));
        assert_eq!(
            eval!("Map(Fn([a, b], [a, b]), [1, 2, 3], 4)"),
            v(&[v(&[i(1), i(4)]), v(&[i(2), i(4)]), v(&[i(3), i(4)])])
        );
        assert_eq!(eval!("Fold(Fn([a, b], a + b), [1, 2, 3])"), i(6));
        assert_eq!(eval!("Fold(Fn([a, b], a + 1), ['a', 'b', 'c', 'd'], 0)"), i(4));
        assert_eq!(eval!("Filter(Fn([a], a > 5), [2, 4, 6, 8])"), v(&[i(6), i(8)]));
        assert_eq!(eval!("Count([1, 'a', 3, 2])"), i(4));
        Ok(())
    }

    #[fuchsia::test]
    fn test_now() -> Result<(), Error> {
        let now_expression = parse_expression("Now()")?;
        let values = HashMap::new();
        let fetcher = Fetcher::TrialData(TrialDataFetcher::new(&values));
        let files = HashMap::new();
        let state = MetricState::new(&files, fetcher, Some(2000));

        let time = state.evaluate_expression(&now_expression);
        let no_time = state.evaluate_expression(&parse_expression("Now(5)")?);
        assert_eq!(time, i(2000));
        assert_problem!(no_time, "SyntaxError: Now() requires no operands.");
        Ok(())
    }

    #[fuchsia::test]
    fn test_option() {
        // Should "Every value was missing" be a ValueError or a Missing?
        assert_problem!(eval!("Option()"), "Missing: Every value was missing");
        // Now() will return Problem::Missing.
        assert_problem!(
            eval!("Option(Now(), Now(), Now(), Now())"),
            "Missing: Every value was missing"
        );
        assert_eq!(eval!("Option(5)"), i(5));
        assert_eq!(eval!("Option(5, Now())"), i(5));
        assert_eq!(eval!("Option(Now(), 5, Now())"), i(5));
        assert_eq!(eval!("Option(Now(), Now(), Now(), Now(), 5)"), i(5));
        assert_eq!(eval!("Option(Now(), Now(), [], Now())"), MetricValue::Vector(vec![]));
        assert_eq!(eval!("Option(Now(), Now(), [], Now(), [5])"), MetricValue::Vector(vec![i(5)]));
        assert_eq!(eval!("Option(Now(), Now(), 5, Now(), [5])"), i(5));
        assert_eq!(eval!("Option(Now(), Now(), [5], Now(), 5)"), MetricValue::Vector(vec![i(5)]));
    }
}
