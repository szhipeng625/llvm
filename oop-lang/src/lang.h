#pragma once
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <cstdint>

// ===================== 词法单元 =====================
enum class Tk {
  End, Id, LitInt, LitStr,
  KwClass, KwExtends, KwVirtual, KwNew, KwIf, KwElse, KwWhile, KwFor,
  KwReturn, KwInt, KwString, KwVoid, KwBool, KwVector, KwThis,
  KwTrue, KwFalse, KwNull, KwPrint,
  LP, RP, LBrace, RBrace, LBracket, RBracket,
  Lt, Gt, Le, Ge, EqEq, NotEq, Assign,
  Plus, Minus, Star, Slash, Percent,
  Dot, Comma, Semi, AndAnd, OrOr, Not
};

struct Token {
  Tk kind = Tk::End;
  std::string text;
  long long num = 0;
  int line = 1;
};

class Lexer {
public:
  explicit Lexer(std::string src) : src_(std::move(src)) {}
  std::vector<Token> run();
private:
  std::string src_;
  size_t pos_ = 0;
  int line_ = 1;
};

// ===================== 抽象语法树 =====================
struct Expr;
using ExprP = std::unique_ptr<Expr>;

struct Expr {
  enum K { LitInt, LitStr, LitBool, LitNull, Var, ThisE, NewE, NewVector,
           FieldE, MethodE, CallE, IndexE, Bin, Un } k = LitInt;
  long long num = 0;
  bool bval = false;
  std::string text;   // 变量名/字符串/字段名/方法名/函数名/运算符
  std::string ty;     // vector 元素类型 或 new 的类名
  ExprP a, b;
  std::vector<ExprP> args;
  int line = 0;
};

struct Stmt;
using StmtP = std::unique_ptr<Stmt>;

struct Stmt {
  enum K { VarDecl, Assign, ExprStmt, IfS, WhileS, ForS, ReturnS, BlockS } k = VarDecl;
  std::string ty, name;   // 变量声明的类型与名字
  ExprP val;              // 声明初值 / 条件 / 返回值
  ExprP target;           // 赋值左值
  StmtP init;             // for 初始化
  ExprP step;             // for 步进
  std::vector<StmtP> body, alt;
  int line = 0;
};

struct Method {
  std::string retTy, name;
  bool isVirtual = false;
  std::vector<std::pair<std::string,std::string>> params; // 参数(类型, 名字)
  std::vector<StmtP> body;
};

struct Field {
  std::string ty, name;
};

struct ClassDecl {
  std::string name, base;
  bool hasBase = false;
  std::vector<Field> ownFields;
  std::vector<Method> methods;
  int line = 0;
};

struct Program {
  std::vector<ClassDecl> classes;
  std::vector<StmtP> mainBody;
};

// ===================== 语法分析 =====================
class Parser {
public:
  explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}
  Program parse();
private:
  std::vector<Token> toks_;
  size_t i_ = 0;
  [[noreturn]] void err(const std::string& msg);
  const Token& cur();
  const Token& peek(int n = 1);
  bool accept(Tk k);
  Token expect(Tk k, const char* what);
  bool isTypeStart();
  std::string parseType();
  ClassDecl parseClass();
  Method parseMethod(const std::string& retTy, bool virt);
  std::vector<StmtP> parseBlock();
  StmtP parseStmt();
  ExprP parseExpr();
  ExprP parseOr();
  ExprP parseAnd();
  ExprP parseCmp();
  ExprP parseAdd();
  ExprP parseMul();
  ExprP parseUnary();
  ExprP parsePostfix();
  ExprP parsePrimary();
  std::vector<ExprP> parseArgs();
};

// ===================== 代码生成 =====================
int runCodeGen(const Program& prog, const std::string& outIr, const std::string& outObj);
