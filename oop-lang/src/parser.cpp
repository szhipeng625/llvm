#include "lang.h"
#include <stdexcept>

[[noreturn]] void Parser::err(const std::string& msg) {
  throw std::runtime_error("第 " + std::to_string(cur().line) + " 行: " + msg);
}
const Token& Parser::cur() { return toks_[i_]; }
const Token& Parser::peek(int n) {
  size_t j = i_ + n;
  if (j >= toks_.size()) j = toks_.size() - 1;
  return toks_[j];
}
bool Parser::accept(Tk k) { if (cur().kind == k) { i_++; return true; } return false; }
Token Parser::expect(Tk k, const char* what) {
  if (cur().kind != k) err(std::string("期望 ") + what);
  return toks_[i_++];
}

bool Parser::isTypeStart() {
  Tk k = cur().kind;
  if (k == Tk::KwInt || k == Tk::KwString || k == Tk::KwBool || k == Tk::KwVector) return true;
  // 类类型: 形如 Docker d;  —— 两个相邻的标识符
  if (k == Tk::Id && peek().kind == Tk::Id) return true;
  return false;
}

std::string Parser::parseType() {
  if (accept(Tk::KwVector)) {
    expect(Tk::Lt, "<");
    std::string elem = parseType();
    expect(Tk::Gt, ">");
    return "vector<" + elem + ">";
  }
  if (accept(Tk::KwInt)) return "int";
  if (accept(Tk::KwString)) return "string";
  if (accept(Tk::KwBool)) return "bool";
  if (accept(Tk::KwVoid)) return "void";
  if (cur().kind == Tk::Id) return expect(Tk::Id, "类型名").text;
  err("期望类型");
}

std::vector<ExprP> Parser::parseArgs() {
  std::vector<ExprP> out;
  expect(Tk::LP, "(");
  if (!accept(Tk::RP)) {
    out.push_back(parseExpr());
    while (accept(Tk::Comma)) out.push_back(parseExpr());
    expect(Tk::RP, ")");
  }
  return out;
}

Program Parser::parse() {
  Program p;
  while (cur().kind != Tk::End) {
    if (cur().kind == Tk::KwClass) {
      p.classes.push_back(parseClass());
    } else {
      p.mainBody.push_back(parseStmt());
    }
  }
  return p;
}

ClassDecl Parser::parseClass() {
  ClassDecl c;
  c.line = cur().line;
  expect(Tk::KwClass, "class");
  c.name = expect(Tk::Id, "类名").text;
  if (accept(Tk::KwExtends)) { c.base = expect(Tk::Id, "父类名").text; c.hasBase = true; }
  expect(Tk::LBrace, "{");
  while (!accept(Tk::RBrace)) {
    bool virt = accept(Tk::KwVirtual);
    std::string retTy = parseType();
    std::string mname = expect(Tk::Id, "成员名").text;
    if (cur().kind == Tk::LP) {
      c.methods.push_back(parseMethod(retTy, virt));
      c.methods.back().name = mname;
      c.methods.back().retTy = retTy;
    } else {
      if (virt) err("virtual 只能用于方法");
      Field f; f.ty = retTy; f.name = mname;
      c.ownFields.push_back(f);
      expect(Tk::Semi, ";");
    }
  }
  return c;
}

Method Parser::parseMethod(const std::string& retTy, bool virt) {
  Method m;
  m.retTy = retTy;
  m.isVirtual = virt;
  expect(Tk::LP, "(");
  if (!accept(Tk::RP)) {
    do {
      std::string pt = parseType();
      std::string pn = expect(Tk::Id, "参数名").text;
      m.params.push_back({pt, pn});
    } while (accept(Tk::Comma));
    expect(Tk::RP, ")");
  }
  m.body = parseBlock();
  return m;
}

std::vector<StmtP> Parser::parseBlock() {
  expect(Tk::LBrace, "{");
  std::vector<StmtP> out;
  while (cur().kind != Tk::RBrace) {
    if (cur().kind == Tk::End) err("缺少 }");
    out.push_back(parseStmt());
  }
  expect(Tk::RBrace, "}");
  return out;
}

StmtP Parser::parseStmt() {
  StmtP s = std::make_unique<Stmt>();
  s->line = cur().line;
  if (cur().kind == Tk::KwIf) {
    i_++;
    s->k = Stmt::IfS;
    expect(Tk::LP, "(");
    s->val = parseExpr();
    expect(Tk::RP, ")");
    s->body = parseBlock();
    if (accept(Tk::KwElse)) s->alt = parseBlock();
    return s;
  }
  if (cur().kind == Tk::KwWhile) {
    i_++;
    s->k = Stmt::WhileS;
    expect(Tk::LP, "(");
    s->val = parseExpr();
    expect(Tk::RP, ")");
    s->body = parseBlock();
    return s;
  }
  if (cur().kind == Tk::KwFor) {
    i_++;
    s->k = Stmt::ForS;
    expect(Tk::LP, "(");
    s->init = parseStmt();
    s->val = parseExpr();
    expect(Tk::Semi, ";");
    s->step = parseExpr();
    expect(Tk::RP, ")");
    s->body = parseBlock();
    return s;
  }
  if (cur().kind == Tk::KwReturn) {
    i_++;
    s->k = Stmt::ReturnS;
    if (cur().kind != Tk::Semi) s->val = parseExpr();
    expect(Tk::Semi, ";");
    return s;
  }
  if (cur().kind == Tk::LBrace) {
    s->k = Stmt::BlockS;
    s->body = parseBlock();
    return s;
  }
  if (isTypeStart()) {
    s->k = Stmt::VarDecl;
    s->ty = parseType();
    s->name = expect(Tk::Id, "变量名").text;
    if (accept(Tk::Assign)) s->val = parseExpr();
    expect(Tk::Semi, ";");
    return s;
  }
  // 赋值或表达式语句
  ExprP first = parseExpr();
  if (accept(Tk::Assign)) {
    s->k = Stmt::Assign;
    s->target = std::move(first);
    s->val = parseExpr();
  } else {
    s->k = Stmt::ExprStmt;
    s->val = std::move(first);
  }
  expect(Tk::Semi, ";");
  return s;
}

ExprP Parser::parseExpr() { return parseOr(); }

ExprP Parser::parseOr() {
  ExprP a = parseAnd();
  while (cur().kind == Tk::OrOr) {
    std::string op = cur().text; i_++;
    ExprP b = parseAnd();
    auto e = std::make_unique<Expr>(); e->k = Expr::Bin; e->text = op; e->a = std::move(a); e->b = std::move(b);
    a = std::move(e);
  }
  return a;
}
ExprP Parser::parseAnd() {
  ExprP a = parseCmp();
  while (cur().kind == Tk::AndAnd) {
    std::string op = cur().text; i_++;
    ExprP b = parseCmp();
    auto e = std::make_unique<Expr>(); e->k = Expr::Bin; e->text = op; e->a = std::move(a); e->b = std::move(b);
    a = std::move(e);
  }
  return a;
}
ExprP Parser::parseCmp() {
  ExprP a = parseAdd();
  while (cur().kind == Tk::Lt || cur().kind == Tk::Gt || cur().kind == Tk::Le || cur().kind == Tk::Ge ||
         cur().kind == Tk::EqEq || cur().kind == Tk::NotEq) {
    std::string op = cur().text; i_++;
    ExprP b = parseAdd();
    auto e = std::make_unique<Expr>(); e->k = Expr::Bin; e->text = op; e->a = std::move(a); e->b = std::move(b);
    a = std::move(e);
  }
  return a;
}
ExprP Parser::parseAdd() {
  ExprP a = parseMul();
  while (cur().kind == Tk::Plus || cur().kind == Tk::Minus) {
    std::string op = cur().text; i_++;
    ExprP b = parseMul();
    auto e = std::make_unique<Expr>(); e->k = Expr::Bin; e->text = op; e->a = std::move(a); e->b = std::move(b);
    a = std::move(e);
  }
  return a;
}
ExprP Parser::parseMul() {
  ExprP a = parseUnary();
  while (cur().kind == Tk::Star || cur().kind == Tk::Slash || cur().kind == Tk::Percent) {
    std::string op = cur().text; i_++;
    ExprP b = parseUnary();
    auto e = std::make_unique<Expr>(); e->k = Expr::Bin; e->text = op; e->a = std::move(a); e->b = std::move(b);
    a = std::move(e);
  }
  return a;
}
ExprP Parser::parseUnary() {
  if (cur().kind == Tk::Not || cur().kind == Tk::Minus) {
    std::string op = cur().text; i_++;
    ExprP v = parseUnary();
    auto e = std::make_unique<Expr>(); e->k = Expr::Un; e->text = op; e->a = std::move(v);
    return e;
  }
  return parsePostfix();
}

ExprP Parser::parsePostfix() {
  ExprP e = parsePrimary();
  while (true) {
    if (cur().kind == Tk::Dot) {
      i_++;
      std::string name = expect(Tk::Id, "成员名").text;
      if (cur().kind == Tk::LP) {
        auto n = std::make_unique<Expr>(); n->k = Expr::MethodE; n->text = name; n->a = std::move(e);
        n->args = parseArgs();
        e = std::move(n);
      } else {
        auto n = std::make_unique<Expr>(); n->k = Expr::FieldE; n->text = name; n->a = std::move(e);
        e = std::move(n);
      }
    } else if (cur().kind == Tk::LP) {
      auto n = std::make_unique<Expr>(); n->k = Expr::CallE; n->a = std::move(e);
      n->args = parseArgs();
      e = std::move(n);
    } else if (cur().kind == Tk::LBracket) {
      i_++;
      auto n = std::make_unique<Expr>(); n->k = Expr::IndexE; n->a = std::move(e);
      n->b = parseExpr();
      expect(Tk::RBracket, "]");
      e = std::move(n);
    } else break;
  }
  return e;
}

ExprP Parser::parsePrimary() {
  auto e = std::make_unique<Expr>();
  e->line = cur().line;
  switch (cur().kind) {
    case Tk::LitInt: e->k = Expr::LitInt; e->num = cur().num; i_++; return e;
    case Tk::LitStr: e->k = Expr::LitStr; e->text = cur().text; i_++; return e;
    case Tk::KwTrue:  e->k = Expr::LitBool; e->bval = true; i_++; return e;
    case Tk::KwFalse: e->k = Expr::LitBool; e->bval = false; i_++; return e;
    case Tk::KwNull:  e->k = Expr::LitNull; i_++; return e;
    case Tk::KwThis:  e->k = Expr::ThisE; i_++; return e;
    case Tk::KwPrint: {
      i_++;
      auto n = std::make_unique<Expr>(); n->k = Expr::CallE; n->line = e->line;
      auto callee = std::make_unique<Expr>(); callee->k = Expr::Var; callee->text = "print";
      n->a = std::move(callee);
      n->args = parseArgs();
      return n;
    }
    case Tk::KwNew: {
      i_++;
      if (accept(Tk::KwVector)) {
        auto n = std::make_unique<Expr>(); n->k = Expr::NewVector; n->line = e->line;
        expect(Tk::Lt, "<");
        n->ty = parseType();
        expect(Tk::Gt, ">");
        expect(Tk::LP, "("); expect(Tk::RP, ")");
        return n;
      }
      std::string cls = expect(Tk::Id, "类名").text;
      auto n = std::make_unique<Expr>(); n->k = Expr::NewE; n->ty = cls; n->line = e->line;
      n->args = parseArgs();
      return n;
    }
    case Tk::LP: {
      i_++;
      ExprP inner = parseExpr();
      expect(Tk::RP, ")");
      return inner;
    }
    case Tk::Id: {
      e->k = Expr::Var; e->text = cur().text; i_++; return e;
    }
    default:
      err("无法解析的表达式");
  }
}
