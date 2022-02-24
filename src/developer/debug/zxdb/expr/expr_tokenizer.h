// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_EXPR_EXPR_TOKENIZER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_EXPR_EXPR_TOKENIZER_H_

#include <string>
#include <string_view>
#include <vector>

#include "src/developer/debug/zxdb/common/err.h"
#include "src/developer/debug/zxdb/expr/expr_language.h"
#include "src/developer/debug/zxdb/expr/expr_token.h"
#include "src/developer/debug/zxdb/symbols/dwarf_lang.h"

namespace zxdb {

class ExprTokenizer {
 public:
  explicit ExprTokenizer(const std::string& input, ExprLanguage lang = ExprLanguage::kC);

  // Returns true on successful tokenizing. In this case, the tokens can be read from tokens(). On
  // failure, err() will contain the error message, and error_location() will contain the error
  // location.
  bool Tokenize();

  const std::string& input() const { return input_; }

  ExprLanguage language() const { return language_; }

  // The result of parsing. This will be multiline and will indicate the location of the problem.
  const Err& err() const { return err_; }

  // When err is set, this will be the index into the input() string where the
  // error occurred.
  size_t error_location() const { return error_location_; }

  // When parsing is successful, this contains the extracted tokens.
  const std::vector<ExprToken>& tokens() const { return tokens_; }

  std::vector<ExprToken> TakeTokens() { return std::move(tokens_); }

  // Returns the number of bytes that start at the given input that are valid name tokens.
  // If the input does not begin with a name token, this will return 0.
  static size_t GetNameTokenLength(ExprLanguage lang, std::string_view input);

  // Returns whether the input is a valid unescaped name token. This does no trimming of whitespace
  // and does not accept "$" escaping. An empty string is not a valid name token.
  static bool IsNameToken(ExprLanguage lang, std::string_view input);

  // Returns two context lines for an error message. It will quote a relevant portion of the input
  // showing the byte offset, and add a "^" on the next line to indicate where the error is.
  static std::string GetErrorContext(const std::string& input, size_t byte_offset);

 private:
  void AdvanceChars(int n);
  void AdvanceOneChar();
  void AdvanceToNextToken();
  void AdvanceToEndOfToken(const ExprTokenRecord& record);

  bool IsCurrentWhitespace() const;

  // Returns true if the next characters in the buffer match the static value of the given token
  // record. If the token is alphanumeric, requires that the end of the token be nonalphanumeric.
  bool CurrentMatchesTokenRecord(const ExprTokenRecord& record) const;

  const ExprTokenRecord& ClassifyCurrent();

  // Checks for a comment beginning at the cur_char(). If it is one, appends a token for the entire
  // comment contents and returns true. Returns false if a comment does not begin here.
  bool HandleComment();

  bool done() const { return at_end() || has_error(); }
  bool has_error() const { return err_.has_error(); }
  bool at_end() const { return cur_ == input_.size(); }
  char cur_char() const { return input_[cur_]; }
  bool can_advance(int n) const { return cur_ + n <= input_.size(); }

  std::string input_;
  ExprLanguage language_;

  size_t cur_ = 0;  // Character offset into input_.

  Err err_;
  size_t error_location_ = 0;

  std::vector<ExprToken> tokens_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_EXPR_EXPR_TOKENIZER_H_
