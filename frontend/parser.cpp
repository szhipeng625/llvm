#include "parser.h"

#include "lexer.h"  // SourceError

#include <string>

namespace pylite {

namespace {

// 词法层没有 "+=" 这样的 token,`a += b` 会切成 Plus 再 Assign。
// 所以在二元运算符的循环里必须检查"下一个是不是 =",否则 `a += b` 会被
// 当成 `a + (= b)` 解析并在 '=' 处报错。
bool isAugAssignPair(Tok cur, Tok next) {
  if (next != Tok::Assign) return false;
  switch (cur) {
    case Tok::Plus:
    case Tok::Minus:
    case Tok::Star:
    case Tok::Slash:
    case Tok::DoubleSlash:
    case Tok::Percent:
    case Tok::StarStar:
      return true;
    default:
      return false;
  }
}

const char *augOpText(Tok k) {
  switch (k) {
    case Tok::Plus:        return "+";
    case Tok::Minus:       return "-";
    case Tok::Star:        return "*";
    case Tok::Slash:       return "/";
    case Tok::DoubleSlash: return "//";
    case Tok::Percent:     return "%";
    case Tok::StarStar:    return "**";
    default:               return "?";
  }
}

}  // namespace

Parser::Parser(std::vector<Token> toks, std::string filename)
    : toks_(std::move(toks)), file_(std::move(filename)) {}

const Token &Parser::peek(size_t ahead) const {
  size_t i = pos_ + ahead;
  // 最后一个 token 一定是 EndOfFile,越界时钳到它,调用方不必自己防越界
  if (i >= toks_.size()) i = toks_.size() - 1;
  return toks_[i];
}

bool Parser::accept(Tok k) {
  if (at(k)) {
    ++pos_;
    return true;
  }
  return false;
}

void Parser::failAt(const Token &t, const std::string &msg) const {
  throw SourceError(t.line, t.col,
                    file_ + ":" + std::to_string(t.line) + ":" + std::to_string(t.col) +
                        ": " + msg);
}

void Parser::fail(const std::string &msg) const { failAt(cur(), msg); }

const Token &Parser::expect(Tok k, const char *what) {
  if (!at(k)) {
    std::string msg = std::string("期望 ") + what + ",实际是 '" +
                      tokName(cur().kind) + "'";
    if (!cur().text.empty() && cur().text != "\n") msg += " (" + cur().text + ")";
    fail(msg);
  }
  return toks_[pos_++];
}

std::unique_ptr<Module> Parser::parseModule() {
  auto m = std::make_unique<Module>();
  while (at(Tok::Newline)) ++pos_;
  while (!at(Tok::EndOfFile)) {
    m->body.push_back(parseStmt());
    while (at(Tok::Newline)) ++pos_;  // 语句之间的空行
  }
  return m;
}

// ---------------------------------------------------------------------------
// 语句
// ---------------------------------------------------------------------------

std::vector<StmtPtr> Parser::parseBlock() {
  expect(Tok::Indent, "缩进块(这一行之后需要有一个缩进的代码块)");
  std::vector<StmtPtr> body;
  while (!at(Tok::Dedent) && !at(Tok::EndOfFile)) {
    body.push_back(parseStmt());
    while (at(Tok::Newline)) ++pos_;
  }
  accept(Tok::Dedent);
  return body;
}

StmtPtr Parser::parseStmt() {
  switch (cur().kind) {
    case Tok::KwDef:    return parseFuncDef();
    case Tok::KwIf:     return parseIf();
    case Tok::KwWhile:  return parseWhile();
    case Tok::KwFor:    return parseFor();
    case Tok::KwReturn: return parseReturn();
    case Tok::KwBreak: {
      const Token &t = cur();
      ++pos_;
      expect(Tok::Newline, "行尾");
      auto s = std::make_unique<Break>();
      s->line = t.line;
      s->col = t.col;
      return s;
    }
    case Tok::KwContinue: {
      const Token &t = cur();
      ++pos_;
      expect(Tok::Newline, "行尾");
      auto s = std::make_unique<Continue>();
      s->line = t.line;
      s->col = t.col;
      return s;
    }
    default:
      return parseSimpleStmt();
  }
}

StmtPtr Parser::parseSimpleStmt() {
  const int line = cur().line;
  const int col = cur().col;

  auto first = parseExprList();

  // 复合赋值:a += b。必须先判断,否则 `=` 会被当成普通赋值的开始。
  if (isAugAssignPair(cur().kind, peek(1).kind)) {
    if (first.size() != 1) fail("复合赋值的左侧只能是单个变量");
    const std::string op = augOpText(cur().kind);
    pos_ += 2;  // 吃掉运算符和 '='
    auto rhs = parseExpr();
    expect(Tok::Newline, "行尾");
    auto s = std::make_unique<AugAssign>();
    s->line = line;
    s->col = col;
    s->target = std::move(first[0]);
    s->op = op;
    s->value = std::move(rhs);
    return s;
  }

  // 赋值。注意单目标和多目标走同一条路径 —— `a, b = ...` 的 targets 有多个,
  // `a = ...` 只有一个。IRGen 那边统一按多值解包处理。
  if (accept(Tok::Assign)) {
    auto rhs = parseExprList();
    expect(Tok::Newline, "行尾");
    auto s = std::make_unique<Assign>();
    s->line = line;
    s->col = col;
    for (auto &e : first) s->targets.push_back(std::move(e));
    s->value = wrapList(std::move(rhs), line, col);
    return s;
  }

  expect(Tok::Newline, "行尾");
  auto s = std::make_unique<ExprStmt>();
  s->line = line;
  s->col = col;
  s->expr = wrapList(std::move(first), line, col);
  return s;
}

StmtPtr Parser::parseFuncDef() {
  const Token &kw = cur();
  expect(Tok::KwDef, "'def'");
  const Token &name = expect(Tok::Identifier, "函数名");

  auto f = std::make_unique<FuncDef>();
  f->line = kw.line;
  f->col = kw.col;
  f->name = name.text;

  expect(Tok::LParen, "'('");
  if (!at(Tok::RParen)) {
    for (;;) {
      const Token &p = expect(Tok::Identifier, "参数名");
      TypeName ty = TypeName::Any;
      if (accept(Tok::Colon)) ty = parseTypeAnnotation();
      f->params.emplace_back(p.text, ty);
      if (!accept(Tok::Comma)) break;
      if (at(Tok::RParen)) break;
    }
  }
  expect(Tok::RParen, "')'");
  if (accept(Tok::Arrow)) f->retType = parseTypeAnnotation();

  expect(Tok::Colon, "':'");
  expect(Tok::Newline, "行尾");
  f->body = parseBlock();
  return f;
}

StmtPtr Parser::parseIf() {
  const Token &kw = cur();
  auto s = std::make_unique<If>();
  s->line = kw.line;
  s->col = kw.col;

  expect(Tok::KwIf, "'if'");
  s->cond = parseExpr();
  expect(Tok::Colon, "':'");
  expect(Tok::Newline, "行尾");
  s->body = parseBlock();

  while (at(Tok::KwElif)) {
    ++pos_;
    auto c = parseExpr();
    expect(Tok::Colon, "':'");
    expect(Tok::Newline, "行尾");
    auto b = parseBlock();
    s->elifs.emplace_back(std::move(c), std::move(b));
  }

  if (accept(Tok::KwElse)) {
    expect(Tok::Colon, "':'");
    expect(Tok::Newline, "行尾");
    s->orelse = parseBlock();
  }
  return s;
}

StmtPtr Parser::parseWhile() {
  const Token &kw = cur();
  auto s = std::make_unique<While>();
  s->line = kw.line;
  s->col = kw.col;

  expect(Tok::KwWhile, "'while'");
  s->cond = parseExpr();
  expect(Tok::Colon, "':'");
  expect(Tok::Newline, "行尾");
  s->body = parseBlock();
  return s;
}

StmtPtr Parser::parseFor() {
  const Token &kw = cur();
  auto s = std::make_unique<For>();
  s->line = kw.line;
  s->col = kw.col;

  expect(Tok::KwFor, "'for'");
  const Token &v = expect(Tok::Identifier, "循环变量名");
  s->var = v.text;
  expect(Tok::KwIn, "'in'");
  s->iterable = parseExpr();
  expect(Tok::Colon, "':'");
  expect(Tok::Newline, "行尾");
  s->body = parseBlock();
  return s;
}

StmtPtr Parser::parseReturn() {
  const Token &kw = cur();
  auto s = std::make_unique<Return>();
  s->line = kw.line;
  s->col = kw.col;

  expect(Tok::KwReturn, "'return'");
  if (!at(Tok::Newline)) {
    auto items = parseExprList();
    s->value = wrapList(std::move(items), kw.line, kw.col);
  }
  expect(Tok::Newline, "行尾");
  return s;
}

// ---------------------------------------------------------------------------
// 表达式
// ---------------------------------------------------------------------------

std::vector<ExprPtr> Parser::parseExprList() {
  std::vector<ExprPtr> items;
  items.push_back(parseExpr());
  while (accept(Tok::Comma)) {
    // 允许尾随逗号,以及 `a, b = ...` 里逗号后直接跟 '=' 的情况
    if (at(Tok::Newline) || at(Tok::Assign) || at(Tok::RParen) ||
        at(Tok::RBracket) || at(Tok::RBrace) || at(Tok::Colon)) {
      break;
    }
    items.push_back(parseExpr());
  }
  return items;
}

ExprPtr Parser::parseExpr() { return parseOr(); }

ExprPtr Parser::parseOr() {
  auto lhs = parseAnd();
  while (at(Tok::KwOr)) {
    const Token &t = cur();
    ++pos_;
    auto rhs = parseAnd();
    auto e = std::make_unique<BinOp>();
    e->op = "or";
    e->line = t.line;
    e->col = t.col;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    lhs = std::move(e);
  }
  return lhs;
}

ExprPtr Parser::parseAnd() {
  auto lhs = parseNot();
  while (at(Tok::KwAnd)) {
    const Token &t = cur();
    ++pos_;
    auto rhs = parseNot();
    auto e = std::make_unique<BinOp>();
    e->op = "and";
    e->line = t.line;
    e->col = t.col;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    lhs = std::move(e);
  }
  return lhs;
}

ExprPtr Parser::parseNot() {
  if (at(Tok::KwNot)) {
    const Token &t = cur();
    ++pos_;
    auto e = std::make_unique<UnaryOp>();
    e->op = "not";
    e->line = t.line;
    e->col = t.col;
    e->operand = parseNot();  // 递归,所以 `not not a` 合法
    return e;
  }
  return parseComparison();
}

ExprPtr Parser::parseComparison() {
  auto lhs = parseAdd();
  for (;;) {
    const char *op = nullptr;
    switch (cur().kind) {
      case Tok::EqEq: op = "=="; break;
      case Tok::NotEq: op = "!="; break;
      case Tok::Lt:   op = "<";  break;
      case Tok::Le:   op = "<="; break;
      case Tok::Gt:   op = ">";  break;
      case Tok::Ge:   op = ">="; break;
      default: return lhs;
    }
    const Token &t = cur();
    ++pos_;
    auto rhs = parseAdd();
    auto e = std::make_unique<BinOp>();
    e->op = op;
    e->line = t.line;
    e->col = t.col;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    lhs = std::move(e);  // 左结合。注意 Python 的链式比较(a < b < c)这里不支持
  }
}

ExprPtr Parser::parseAdd() {
  auto lhs = parseMul();
  for (;;) {
    if (isAugAssignPair(cur().kind, peek(1).kind)) return lhs;
    if (!at(Tok::Plus) && !at(Tok::Minus)) return lhs;
    const Token &t = cur();
    const char *op = (t.kind == Tok::Plus) ? "+" : "-";
    ++pos_;
    auto rhs = parseMul();
    auto e = std::make_unique<BinOp>();
    e->op = op;
    e->line = t.line;
    e->col = t.col;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    lhs = std::move(e);
  }
}

ExprPtr Parser::parseMul() {
  auto lhs = parseUnary();
  for (;;) {
    if (isAugAssignPair(cur().kind, peek(1).kind)) return lhs;
    const char *op = nullptr;
    switch (cur().kind) {
      case Tok::Star:        op = "*";  break;
      case Tok::Slash:       op = "/";  break;
      case Tok::DoubleSlash: op = "//"; break;
      case Tok::Percent:     op = "%";  break;
      default: return lhs;
    }
    const Token &t = cur();
    ++pos_;
    auto rhs = parseUnary();
    auto e = std::make_unique<BinOp>();
    e->op = op;
    e->line = t.line;
    e->col = t.col;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    lhs = std::move(e);
  }
}

ExprPtr Parser::parseUnary() {
  if (at(Tok::Minus)) {
    const Token &t = cur();
    ++pos_;
    auto e = std::make_unique<UnaryOp>();
    e->op = "-";
    e->line = t.line;
    e->col = t.col;
    e->operand = parseUnary();
    return e;
  }
  return parsePower();
}

ExprPtr Parser::parsePower() {
  auto base = parsePostfix();
  if (!isAugAssignPair(cur().kind, peek(1).kind) && at(Tok::StarStar)) {
    const Token &t = cur();
    ++pos_;
    auto e = std::make_unique<BinOp>();
    e->op = "**";
    e->line = t.line;
    e->col = t.col;
    e->lhs = std::move(base);
    // 右侧递归到 parseUnary:既实现右结合,又让 `-2 ** 2` 解析成 `-(2 ** 2)`
    e->rhs = parseUnary();
    return e;
  }
  return base;
}

ExprPtr Parser::parsePostfix() {
  auto e = parseAtom();
  for (;;) {
    if (at(Tok::LParen)) {
      const Token &lp = cur();
      ++pos_;
      auto c = std::make_unique<Call>();
      c->line = lp.line;
      c->col = lp.col;
      c->callee = std::move(e);
      if (!at(Tok::RParen)) {
        for (;;) {
          c->args.push_back(parseExpr());
          if (!accept(Tok::Comma)) break;
          if (at(Tok::RParen)) break;  // 尾随逗号
        }
      }
      expect(Tok::RParen, "')'");
      e = std::move(c);
    } else if (at(Tok::LBracket)) {
      e = parseSubscriptOf(std::move(e));
    } else if (at(Tok::Dot)) {
      ++pos_;
      const Token &nameTok = expect(Tok::Identifier, "属性名");
      auto a = std::make_unique<Attribute>();
      a->line = nameTok.line;
      a->col = nameTok.col;
      a->name = nameTok.text;
      a->obj = std::move(e);
      e = std::move(a);
    } else {
      return e;
    }
  }
}

ExprPtr Parser::parseSubscriptOf(ExprPtr obj) {
  const Token &lb = cur();
  expect(Tok::LBracket, "'['");

  ExprPtr lo, hi, step;
  bool isSlice = false;

  if (!at(Tok::Colon)) lo = parseExpr();
  if (accept(Tok::Colon)) {
    isSlice = true;
    if (!at(Tok::Colon) && !at(Tok::RBracket)) hi = parseExpr();
    if (accept(Tok::Colon)) {
      if (!at(Tok::RBracket)) step = parseExpr();
    }
  }
  expect(Tok::RBracket, "']'");

  if (isSlice) {
    auto s = std::make_unique<SliceExpr>();
    s->line = lb.line;
    s->col = lb.col;
    s->obj = std::move(obj);
    s->lo = std::move(lo);
    s->hi = std::move(hi);
    s->step = std::move(step);
    return s;
  }

  auto s = std::make_unique<Subscript>();
  s->line = lb.line;
  s->col = lb.col;
  s->obj = std::move(obj);
  s->index = std::move(lo);
  return s;
}

ExprPtr Parser::parseAtom() {
  const Token &t = cur();

  switch (t.kind) {
    case Tok::IntLit: {
      ++pos_;
      auto e = std::make_unique<IntLit>();
      e->value = t.intValue;
      e->line = t.line;
      e->col = t.col;
      return e;
    }
    case Tok::FloatLit: {
      ++pos_;
      auto e = std::make_unique<FloatLit>();
      e->value = t.floatValue;
      e->line = t.line;
      e->col = t.col;
      return e;
    }
    case Tok::StringLit: {
      ++pos_;
      auto e = std::make_unique<StringLit>();
      e->value = t.text;
      e->line = t.line;
      e->col = t.col;
      return e;
    }
    case Tok::KwTrue:
    case Tok::KwFalse: {
      ++pos_;
      auto e = std::make_unique<BoolLit>();
      e->value = (t.kind == Tok::KwTrue);
      e->line = t.line;
      e->col = t.col;
      return e;
    }
    case Tok::KwNone: {
      ++pos_;
      auto e = std::make_unique<NoneLit>();
      e->line = t.line;
      e->col = t.col;
      return e;
    }
    case Tok::LParen: {
      ++pos_;
      if (accept(Tok::RParen)) {  // 空元组
        auto e = std::make_unique<TupleLit>();
        e->line = t.line;
        e->col = t.col;
        return e;
      }
      auto items = parseExprList();
      expect(Tok::RParen, "')'");
      // (a) 就是 a;(a, b) 才是元组 —— wrapList 负责区分
      return wrapList(std::move(items), t.line, t.col);
    }
    case Tok::LBracket: {
      ++pos_;
      auto e = std::make_unique<ListLit>();
      e->line = t.line;
      e->col = t.col;
      if (!at(Tok::RBracket)) {
        for (;;) {
          e->items.push_back(parseExpr());
          if (!accept(Tok::Comma)) break;
          if (at(Tok::RBracket)) break;
        }
      }
      expect(Tok::RBracket, "']'");
      return e;
    }
    case Tok::LBrace: {
      ++pos_;
      auto e = std::make_unique<DictLit>();
      e->line = t.line;
      e->col = t.col;
      if (!at(Tok::RBrace)) {
        for (;;) {
          auto k = parseExpr();
          expect(Tok::Colon, "':' (字典的键值之间)");
          auto v = parseExpr();
          e->items.emplace_back(std::move(k), std::move(v));
          if (!accept(Tok::Comma)) break;
          if (at(Tok::RBrace)) break;
        }
      }
      expect(Tok::RBrace, "'}'");
      return e;
    }
    case Tok::Identifier: {
      ++pos_;

      // range(...) 单独建节点:IRGen 要把它降成计数循环,而不是普通调用
      if (t.text == "range" && at(Tok::LParen)) {
        ++pos_;
        auto e = std::make_unique<RangeExpr>();
        e->line = t.line;
        e->col = t.col;
        std::vector<ExprPtr> args;
        if (!at(Tok::RParen)) {
          for (;;) {
            args.push_back(parseExpr());
            if (!accept(Tok::Comma)) break;
            if (at(Tok::RParen)) break;
          }
        }
        expect(Tok::RParen, "')'");
        if (args.empty() || args.size() > 3) {
          failAt(t, "range() 需要 1 到 3 个参数,给了 " + std::to_string(args.size()) + " 个");
        }
        if (args.size() == 1) {
          e->stop = std::move(args[0]);
        } else if (args.size() == 2) {
          e->start = std::move(args[0]);
          e->stop = std::move(args[1]);
        } else {
          e->start = std::move(args[0]);
          e->stop = std::move(args[1]);
          e->step = std::move(args[2]);
        }
        return e;
      }

      // input(int, int):参数是类型名,不是表达式,所以单独处理
      if (t.text == "input" && at(Tok::LParen)) {
        ++pos_;
        auto e = std::make_unique<InputExpr>();
        e->line = t.line;
        e->col = t.col;
        if (at(Tok::RParen)) {
          failAt(t, "input() 至少要指定一个类型,例如 input(int, int)");
        }
        for (;;) {
          e->kinds.push_back(parseTypeAnnotation());
          if (!accept(Tok::Comma)) break;
          if (at(Tok::RParen)) break;
        }
        expect(Tok::RParen, "')'");
        return e;
      }

      auto e = std::make_unique<Name>();
      e->id = t.text;
      e->line = t.line;
      e->col = t.col;
      return e;
    }
    default:
      fail("这里需要一个表达式");
  }
}

TypeName Parser::parseTypeAnnotation() {
  switch (cur().kind) {
    case Tok::KwInt:   ++pos_; return TypeName::Int;
    case Tok::KwFloat: ++pos_; return TypeName::Float;
    case Tok::KwBool:  ++pos_; return TypeName::Bool;
    case Tok::KwStr:   ++pos_; return TypeName::Str;
    default:
      fail("这里需要一个类型名(int / float / bool / str)");
  }
}

ExprPtr Parser::wrapList(std::vector<ExprPtr> items, int line, int col) {
  if (items.size() == 1) return std::move(items[0]);
  auto t = std::make_unique<TupleLit>();
  t->line = line;
  t->col = col;
  t->items = std::move(items);
  return t;
}

}  // namespace pylite
