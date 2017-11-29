#include "tile/lang/simplifier.h"

#include "tile/lang/emitc.h"
#include "tile/lang/scope.h"
#include "tile/lang/semtree.h"

namespace vertexai {
namespace tile {

namespace sem {

// This struct holds two totally disparate things, but it's easier
// to use a single scope object throughout the recursive Simplifier
// rather than having to pass multiple scope objects down.
struct Symbol {
  // This member is set if the symbol can be resolved to an IntConst value.
  // If such a value exists, substitute in an IntConst over a LoadExpr.
  boost::optional<int64_t> const_value;

  // This member is set if the symbol can be resolved to another symbol.
  // That is, if the code looks like:
  //   int x = y;
  // Then symbol 'x' is an alias to 'y'.
  // If such an alias exists, substitute every use for 'x' with a LoadExpr of 'y'.
  // It's also safe to elide this declaration since every use of 'x' will be replaced with 'y'.
  boost::optional<std::string> alias;
};

class Simplifier : public Visitor {
 public:
  explicit Simplifier(lang::Scope<Symbol>* scope) : scope_{scope} {}

  void Visit(const IntConst& node) override {}

  void Visit(const FloatConst& node) override {}

  void Visit(const LookupLVal& node) override {
    auto symbol = scope_->Lookup(node.name);
    // Check if a symbol exists that is an alias for another symbol.
    if (symbol && (*symbol).alias) {
      // If such a symbol exists, substitute it in.
      ref_ = *(*symbol).alias;
      const_cast<LookupLVal&>(node).name = ref_;
    } else {
      ref_ = node.name;
    }
  }

  void Visit(const LoadExpr& node) override {
    auto ref = Resolve(node.inner);
    auto symbol = scope_->Lookup(ref);
    // Check if a symbol exists that refers to an IntConst value.
    if (symbol && (*symbol).const_value) {
      // If such a symbol exists, substitute the IntConst expr in directly.
      new_expr_ = std::make_shared<IntConst>(*(*symbol).const_value);
    }
  }

  void Visit(const StoreStmt& node) override {
    Resolve(node.lhs);
    const_cast<StoreStmt&>(node).rhs = EvalExpr(node.rhs);
  }

  void Visit(const SubscriptLVal& node) override {
    ref_ = Resolve(node.ptr);
    const_cast<SubscriptLVal&>(node).offset = EvalExpr(node.offset);
  }

  void Visit(const DeclareStmt& node) override {
    if (node.init) {
      auto init = EvalExpr(node.init);

      auto int_const = std::dynamic_pointer_cast<IntConst>(init);
      if (int_const) {
        Symbol symbol;
        symbol.const_value = int_const->value;
        scope_->Bind(node.name, symbol);
        // Mark this statement as elided.
        new_stmt_ = std::make_shared<Block>();
        return;
      }

      auto load_expr = std::dynamic_pointer_cast<LoadExpr>(init);
      if (load_expr) {
        auto lookup = std::dynamic_pointer_cast<LookupLVal>(load_expr->inner);
        if (lookup) {
          auto ref = Resolve(load_expr->inner);
          Symbol symbol;
          symbol.alias = ref;
          scope_->Bind(node.name, symbol);
          // Mark this statement as elided.
          new_stmt_ = std::make_shared<Block>();
          return;
        }
      }

      const_cast<DeclareStmt&>(node).init = init;
    }
  }

  void Visit(const UnaryExpr& node) override { const_cast<UnaryExpr&>(node).inner = EvalExpr(node.inner); }

  void Visit(const BinaryExpr& node) override {
    const_cast<BinaryExpr&>(node).lhs = EvalExpr(node.lhs);
    const_cast<BinaryExpr&>(node).rhs = EvalExpr(node.rhs);

    if (node.op == "*") {
      if (CheckIntConstValue(node.rhs, 1)) {
        // Check for (L * 1), return (L)
        new_expr_ = node.lhs;
      } else if (CheckIntConstValue(node.lhs, 1)) {
        // Check for (1 * R), return (R)
        new_expr_ = node.rhs;
      } else if (CheckIntConstValue(node.lhs, 0)) {
        // Check for (0 * R), return (0)
        new_expr_ = node.lhs;
      } else if (CheckIntConstValue(node.rhs, 0)) {
        // Check for (L * 0), return (0)
        new_expr_ = node.rhs;
      }
    } else if (node.op == "/") {
      if (CheckIntConstValue(node.rhs, 1)) {
        // Check for (L / 1), return (L)
        new_expr_ = node.lhs;
      } else if (CheckIntConstValue(node.lhs, 0)) {
        // Check for (0 / R), return (0)
        new_expr_ = node.lhs;
      }
    } else if (node.op == "+") {
      if (CheckIntConstValue(node.rhs, 0)) {
        // Check for (L + 0), return (L)
        new_expr_ = node.lhs;
      } else if (CheckIntConstValue(node.lhs, 0)) {
        // Check for (0 + R), return (R)
        new_expr_ = node.rhs;
      }
    } else if (node.op == "-") {
      if (CheckIntConstValue(node.rhs, 0)) {
        // Check for (L - 0), return (L)
        new_expr_ = node.lhs;
      }
    } else if (node.op == "%") {
      if (CheckIntConstValue(node.rhs, 1)) {
        // Check for (L % 1), return (0)
        new_expr_ = std::make_shared<IntConst>(0);
      }
    }
  }

  void Visit(const CondExpr& node) override {
    const_cast<CondExpr&>(node).cond = EvalExpr(node.cond);
    const_cast<CondExpr&>(node).tcase = EvalExpr(node.tcase);
    const_cast<CondExpr&>(node).fcase = EvalExpr(node.fcase);
  }

  void Visit(const SelectExpr& node) override {
    const_cast<SelectExpr&>(node).cond = EvalExpr(node.cond);
    const_cast<SelectExpr&>(node).tcase = EvalExpr(node.tcase);
    const_cast<SelectExpr&>(node).fcase = EvalExpr(node.fcase);
  }

  void Visit(const ClampExpr& node) override {
    const_cast<ClampExpr&>(node).val = EvalExpr(node.val);
    const_cast<ClampExpr&>(node).min = EvalExpr(node.min);
    const_cast<ClampExpr&>(node).max = EvalExpr(node.max);
  }

  void Visit(const CastExpr& node) override { const_cast<CastExpr&>(node).val = EvalExpr(node.val); }

  void Visit(const CallExpr& node) override {
    const_cast<CallExpr&>(node).func = EvalExpr(node.func);
    for (size_t i = 0; i < node.vals.size(); i++) {
      const_cast<CallExpr&>(node).vals[i] = EvalExpr(node.vals[i]);
    }
  }

  void Visit(const LimitConst& node) override {}

  void Visit(const IndexExpr& node) override {}

  void Visit(const Block& node) override {
    lang::Scope<Symbol> scope{scope_};
    auto new_block = std::make_shared<Block>();
    for (const auto& stmt : node.statements) {
      auto new_stmt = EvalStmt(stmt, &scope);
      auto block = std::dynamic_pointer_cast<Block>(new_stmt);
      if (!block || !block->statements.empty()) {
        // Only emit statements that haven't been elided.
        new_block->push_back(new_stmt);
      }
    }
    new_stmt_ = new_block;
  }

  void Visit(const IfStmt& node) override {
    if (node.iffalse) {
      const_cast<IfStmt&>(node).iffalse = EvalStmt(node.iffalse);
    }
    bool invert_test = false;
    if (node.iftrue) {
      const_cast<IfStmt&>(node).iftrue = EvalStmt(node.iftrue);
      auto block = std::dynamic_pointer_cast<Block>(node.iffalse);
      if (block && block->statements.empty()) {
        invert_test = true;
      }
    } else {
      invert_test = true;
    }
    if (invert_test) {
      const_cast<IfStmt&>(node).iftrue = node.iffalse;
      const_cast<IfStmt&>(node).iffalse = StmtPtr();
      const_cast<IfStmt&>(node).cond = std::make_shared<UnaryExpr>("!", node.cond);
    }
    const_cast<IfStmt&>(node).cond = EvalExpr(node.cond);
  }

  void Visit(const ForStmt& node) override { const_cast<ForStmt&>(node).inner = EvalStmt(node.inner); }

  void Visit(const WhileStmt& node) override {
    const_cast<WhileStmt&>(node).cond = EvalExpr(node.cond);
    const_cast<WhileStmt&>(node).inner = EvalStmt(node.inner);
  }

  void Visit(const BarrierStmt& node) override {}

  void Visit(const ReturnStmt& node) override {
    if (node.value) {
      const_cast<ReturnStmt&>(node).value = EvalExpr(node.value);
    }
  }

  void Visit(const Function& node) override { const_cast<Function&>(node).body = EvalStmt(node.body); }

 private:
  bool CheckIntConstValue(const ExprPtr& expr, int64_t value) {
    auto int_const = std::dynamic_pointer_cast<IntConst>(expr);
    return (int_const && int_const->value == value);
  }

  ExprPtr EvalExpr(const ExprPtr& expr) {
    Simplifier eval(scope_);
    expr->Accept(eval);
    if (eval.new_expr_) {
      return eval.new_expr_;
    }
    return expr;
  }

  StmtPtr EvalStmt(const StmtPtr& stmt) { return EvalStmt(stmt, scope_); }

  StmtPtr EvalStmt(const StmtPtr& stmt, lang::Scope<Symbol>* scope) {
    Simplifier eval(scope);
    stmt->Accept(eval);
    if (eval.new_stmt_) {
      auto ifstmt = std::dynamic_pointer_cast<IfStmt>(eval.new_stmt_);
      if (ifstmt && !ifstmt->iftrue) {
        return std::make_shared<Block>();
      }
      return eval.new_stmt_;
    }
    return stmt;
  }

  std::string Resolve(const LValPtr& ptr) {
    Simplifier eval(scope_);
    ptr->Accept(eval);
    return eval.ref_;
  }

 private:
  ExprPtr new_expr_;
  StmtPtr new_stmt_;
  std::string ref_;

  lang::Scope<Symbol>* scope_;
};

}  // namespace sem

namespace lang {

void Simplify(const std::vector<KernelInfo>& kernels) {
  for (const auto& ki : kernels) {
    if (VLOG_IS_ON(4)) {
      lang::EmitDebug emit_debug;
      emit_debug.Visit(*ki.kfunc);
      VLOG(4) << "Generic debug kernel before simplification:";
      VLOG(4) << ki.comments;
      VLOG(4) << emit_debug.str();
    }
    lang::Scope<sem::Symbol> scope;
    sem::Simplifier simplifier{&scope};
    ki.kfunc->Accept(simplifier);
  }
}

}  // namespace lang
}  // namespace tile
}  // namespace vertexai
