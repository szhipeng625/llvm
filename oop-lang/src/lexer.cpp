#include "lang.h"
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <unordered_map>

static const std::unordered_map<std::string, Tk> kKeywords = {
  {"class", Tk::KwClass}, {"extends", Tk::KwExtends}, {"virtual", Tk::KwVirtual},
  {"new", Tk::KwNew}, {"if", Tk::KwIf}, {"else", Tk::KwElse}, {"while", Tk::KwWhile},
  {"for", Tk::KwFor}, {"return", Tk::KwReturn}, {"int", Tk::KwInt},
  {"string", Tk::KwString}, {"void", Tk::KwVoid}, {"bool", Tk::KwBool},
  {"vector", Tk::KwVector}, {"this", Tk::KwThis}, {"true", Tk::KwTrue},
  {"false", Tk::KwFalse}, {"null", Tk::KwNull}, {"print", Tk::KwPrint},
};

std::vector<Token> Lexer::run() {
  std::vector<Token> out;
  while (true) {
    while (pos_ < src_.size()) {
      char c = src_[pos_];
      if (c == '\n') { line_++; pos_++; }
      else if (std::isspace((unsigned char)c)) pos_++;
      else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
        while (pos_ < src_.size() && src_[pos_] != '\n') pos_++;
      } else break;
    }
    if (pos_ >= src_.size()) { Token t; t.kind = Tk::End; t.line = line_; out.push_back(t); break; }
    char c = src_[pos_];
    Token t; t.line = line_;
    if (std::isalpha((unsigned char)c) || c == '_') {
      size_t s = pos_;
      while (pos_ < src_.size() && (std::isalnum((unsigned char)src_[pos_]) || src_[pos_] == '_')) pos_++;
      t.text = src_.substr(s, pos_ - s);
      auto it = kKeywords.find(t.text);
      t.kind = it != kKeywords.end() ? it->second : Tk::Id;
      out.push_back(t); continue;
    }
    if (std::isdigit((unsigned char)c)) {
      size_t s = pos_;
      while (pos_ < src_.size() && std::isdigit((unsigned char)src_[pos_])) pos_++;
      t.text = src_.substr(s, pos_ - s);
      t.num = std::strtoll(t.text.c_str(), nullptr, 10);
      t.kind = Tk::LitInt;
      out.push_back(t); continue;
    }
    if (c == '"') {
      pos_++;
      std::string v;
      while (pos_ < src_.size() && src_[pos_] != '"') {
        char d = src_[pos_++];
        if (d == '\\' && pos_ < src_.size()) {
          char e = src_[pos_++];
          switch (e) {
            case 'n': v += '\n'; break;
            case 't': v += '\t'; break;
            case '"': v += '"'; break;
            case '\\': v += '\\'; break;
            default: v += e;
          }
        } else v += d;
      }
      if (pos_ >= src_.size()) throw std::runtime_error("字符串未闭合");
      pos_++;
      t.text = v; t.kind = Tk::LitStr;
      out.push_back(t); continue;
    }
    auto two = [&](char a, char b, Tk k) -> bool {
      if (c == a && pos_ + 1 < src_.size() && src_[pos_ + 1] == b) {
        pos_ += 2; t.kind = k; t.text = std::string() + a + b; out.push_back(t); return true;
      }
      return false;
    };
    if (two('=', '=', Tk::EqEq)) continue;
    if (two('!', '=', Tk::NotEq)) continue;
    if (two('<', '=', Tk::Le)) continue;
    if (two('>', '=', Tk::Ge)) continue;
    if (two('&', '&', Tk::AndAnd)) continue;
    if (two('|', '|', Tk::OrOr)) continue;
    pos_++;
    switch (c) {
      case '(': t.kind = Tk::LP; break;
      case ')': t.kind = Tk::RP; break;
      case '{': t.kind = Tk::LBrace; break;
      case '}': t.kind = Tk::RBrace; break;
      case '[': t.kind = Tk::LBracket; break;
      case ']': t.kind = Tk::RBracket; break;
      case '<': t.kind = Tk::Lt; break;
      case '>': t.kind = Tk::Gt; break;
      case '=': t.kind = Tk::Assign; break;
      case '+': t.kind = Tk::Plus; break;
      case '-': t.kind = Tk::Minus; break;
      case '*': t.kind = Tk::Star; break;
      case '/': t.kind = Tk::Slash; break;
      case '%': t.kind = Tk::Percent; break;
      case '.': t.kind = Tk::Dot; break;
      case ',': t.kind = Tk::Comma; break;
      case ';': t.kind = Tk::Semi; break;
      case '!': t.kind = Tk::Not; break;
      default: throw std::runtime_error(std::string("无法识别的字符: ") + c);
    }
    t.text = std::string(1, c);
    out.push_back(t);
  }
  return out;
}
