// SPDX-License-Identifier: GPL-3.0-or-later
// Lower the port's DISC_STRUCT accesses in preprocessed C for Clang targets.
// The upstream GCC path remains the reference implementation.
#include <algorithm>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Lex/Lexer.h>
#include <clang/Tooling/Tooling.h>
#include <functional>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <map>
#include <string>
#include <vector>
using namespace clang;
struct Lower : RecursiveASTVisitor<Lower> {
    ASTContext& c;
    SourceManager& sm;
    std::string text;
    bool failed = false;
    struct Rule {
        unsigned a, b;
        std::function<std::string()> emit;
    };
    std::vector<Rule> rules;
    std::vector<int> active;
    std::vector<std::pair<unsigned, unsigned>> initializers;
    Lower(ASTContext& ctx)
        : c(ctx), sm(ctx.getSourceManager()), text(sm.getBufferData(sm.getMainFileID()).str()) {}
    bool disc(const RecordDecl* r) {
        for (auto* a : r->specific_attrs<AnnotateAttr>())
            if (a->getAnnotation() == "melee_disc")
                return true;
        return false;
    }
    unsigned off(SourceLocation l) { return sm.getFileOffset(sm.getSpellingLoc(l)); }
    std::pair<unsigned, unsigned> span(const Stmt* e) {
        return {
            off(e->getBeginLoc()), off(Lexer::getLocForEndOfToken(
                                       sm.getSpellingLoc(e->getEndLoc()), 0, sm, c.getLangOpts()))};
    }
    std::string render(unsigned a, unsigned b, int skip = -1) {
        std::string s;
        unsigned at = a;
        while (at < b) {
            int best = -1;
            for (unsigned i = 0; i < rules.size(); i++) {
                auto& r = rules[i];
                if ((int)i == skip || std::find(active.begin(), active.end(), i) != active.end() ||
                    r.a < at || r.b > b)
                    continue;
                if (best < 0 || r.a < rules[best].a ||
                    (r.a == rules[best].a && r.b > rules[best].b))
                    best = i;
            }
            if (best < 0) {
                s += text.substr(at, b - at);
                break;
            }
            auto& r = rules[best];
            s += text.substr(at, r.a - at);
            active.push_back(best);
            s += r.emit();
            active.pop_back();
            at = r.b;
        }
        return s;
    }
    std::string expr(const Expr* e) {
        auto [a, b] = span(e);
        return render(a, b);
    }
    std::string type(QualType t) { return t.getUnqualifiedType().getAsString(); }
    void error(const Expr* e, const char* m) {
        llvm::errs() << "disc lowering: " << m << " at "
                     << sm.getPresumedLoc(e->getBeginLoc()).getLine() << "\n";
        failed = true;
    }
    struct Access {
        const Expr* object = nullptr;
        const MemberExpr* member = nullptr;
        const FieldDecl* field = nullptr;
        std::vector<const Expr*> indices;
    };
    Access access(const Expr* e) {
        e = e->IgnoreParenImpCasts();
        if (auto* a = dyn_cast<ArraySubscriptExpr>(e)) {
            auto r = access(a->getBase());
            if (r.field)
                r.indices.push_back(a->getIdx());
            return r;
        }
        if (auto* m = dyn_cast<MemberExpr>(e)) {
            auto* f = dyn_cast<FieldDecl>(m->getMemberDecl());
            if (f && disc(f->getParent()))
                return {m->getBase(), m, f, {}};
        }
        return {};
    }
    std::string objaddr(const Expr* e) {
        e = e->IgnoreParens();
        if (auto* m = dyn_cast<MemberExpr>(e)) {
            if (auto* f = dyn_cast<FieldDecl>(m->getMemberDecl());
                f && f->isAnonymousStructOrUnion())
            {
                auto p = m->isArrow() ? "(" + expr(m->getBase()) + ")" : objaddr(m->getBase());
                return "((unsigned char*)" + p + "+" +
                       std::to_string(
                           c.getASTRecordLayout(f->getParent()).getFieldOffset(f->getFieldIndex()) /
                           8) +
                       ")";
            }
        }
        return "&(" + expr(e) + ")";
    }
    std::string address(const Access& a) {
        std::string p = a.member->isArrow() ? "(" + expr(a.object) + ")" : objaddr(a.object);
        auto& layout = c.getASTRecordLayout(a.field->getParent());
        unsigned bit = layout.getFieldOffset(a.field->getFieldIndex());
        std::string out = "((unsigned char*)" + p + "+" + std::to_string(bit / 8) + ")";
        QualType t = a.field->getType();
        for (auto* i : a.indices) {
            auto* ar = c.getAsArrayType(t);
            if (!ar) {
                failed = true;
                break;
            }
            t = ar->getElementType();
            out = "(" + out + "+(" + expr(i) + ")*" +
                  std::to_string(c.getTypeSizeInChars(t).getQuantity()) + ")";
        }
        return out;
    }
    std::string load(const Access& a, QualType t, std::string p = "") {
        if (p.empty())
            p = address(a);
        unsigned bits = c.getTypeSize(t);
        if (a.field->isBitField()) {
            auto& l = c.getASTRecordLayout(a.field->getParent());
            unsigned offset = l.getFieldOffset(a.field->getFieldIndex()) % 8,
                     w = a.field->getBitWidthValue();
            return "((" + type(t) + ")__os_be_bits_get(" + p + "," + std::to_string(offset) + "," +
                   std::to_string(w) + "," + (t->isSignedIntegerType() ? "1" : "0") + "))";
        }
        if (bits == 8)
            return "(*(" + type(t) + "*)" + p + ")";
        return "((" + type(t) + ")__os_be_" + (t->isRealFloatingType() ? "f" : "u") +
               std::to_string(bits) + "(" + p + "))";
    }
    std::string save(const Access& a, QualType t, const std::string& p, const std::string& v) {
        if (a.field->isBitField()) {
            auto& l = c.getASTRecordLayout(a.field->getParent());
            return "__os_be_bits_put(" + p + "," +
                   std::to_string(l.getFieldOffset(a.field->getFieldIndex()) % 8) + "," +
                   std::to_string(a.field->getBitWidthValue()) + "," + v + ")";
        }
        unsigned bits = c.getTypeSize(t);
        if (bits == 8)
            return "(*(" + type(t) + "*)" + p + "=" + v + ")";
        return "__os_be_put_" + (t->isRealFloatingType() ? std::string("f") : std::string("u")) +
               std::to_string(bits) + "(" + p + "," + v + ")";
    }
    void rule(const Expr* e, std::function<std::string()> f) {
        auto [a, b] = span(e);
        rules.push_back({a, b, std::move(f)});
    }
    bool VisitImplicitCastExpr(ImplicitCastExpr* e) {
        if (e->getCastKind() != CK_LValueToRValue)
            return true;
        auto a = access(e->getSubExpr());
        if (a.field && (e->getType()->isArithmeticType() || e->getType()->isEnumeralType()))
            rule(e, [=, this] { return load(a, e->getType()); });
        return true;
    }
    bool VisitBinaryOperator(BinaryOperator* e) {
        if (!e->isAssignmentOp())
            return true;
        auto a = access(e->getLHS());
        if (!a.field || !(e->getLHS()->getType()->isArithmeticType() ||
                            e->getLHS()->getType()->isEnumeralType()))
            return true;
        rule(e, [=, this] {
            auto t = e->getLHS()->getType();
            std::string v = expr(e->getRHS());
            if (e->isCompoundAssignmentOp())
                v = "(" + load(a, t, "__os_p") +
                    std::string(BinaryOperator::getOpcodeStr(
                        BinaryOperator::getOpForCompoundAssignment(e->getOpcode()))) +
                    "(" + v + "))";
            return "({unsigned char*__os_p=" + address(a) + ";" + type(t) + " __os_v=(" + v + ");" +
                   save(a, t, "__os_p", "__os_v") + ";" + load(a, t, "__os_p") + ";})";
        });
        return true;
    }
    bool VisitUnaryOperator(UnaryOperator* e) {
        auto a = access(e->getSubExpr());
        if (!a.field || !(e->getSubExpr()->getType()->isArithmeticType() ||
                            e->getSubExpr()->getType()->isEnumeralType()))
            return true;
        if (e->getOpcode() == UO_AddrOf) {
            error(e, "taking a disc scalar address is unsupported, as in GCC");
            return true;
        }
        if (!e->isIncrementDecrementOp())
            return true;
        rule(e, [=, this] {
            auto t = e->getSubExpr()->getType();
            return "({unsigned char*__os_p=" + address(a) + ";" + type(t) +
                   " __os_old=" + load(a, t, "__os_p") + ";" + type(t) + " __os_v=__os_old" +
                   (e->isIncrementOp() ? "+1" : "-1") + ";" + save(a, t, "__os_p", "__os_v") + ";" +
                   (e->isPostfix() ? "__os_old" : load(a, t, "__os_p")) + ";})";
        });
        return true;
    }
    void initRule(const Expr* e, QualType t) {
        auto location = span(e);
        if (std::find(initializers.begin(), initializers.end(), location) != initializers.end())
            return;
        initializers.push_back(location);
        rule(e, [=, this] { return initScalar(e, t); });
    }
    std::string initScalar(const Expr* e, QualType t) {
        unsigned n = c.getTypeSize(t);
        if (n == 8)
            return expr(e);
        auto v = expr(e);
        if (t->isRealFloatingType())
            return "__builtin_bit_cast(" + type(t) + ",__builtin_bswap" + std::to_string(n) +
                   "(__builtin_bit_cast(uint" + std::to_string(n) + "_t,(" + type(t) + ")(" + v +
                   "))))";
        return "((" + type(t) + ")__builtin_bswap" + std::to_string(n) + "((uint" +
               std::to_string(n) + "_t)(" + v + ")))";
    }
    void initFields(InitListExpr* i, QualType t, bool be = false) {
        if (auto* a = c.getAsArrayType(t)) {
            for (auto* e : i->inits()) {
                if (auto* l = dyn_cast<InitListExpr>(e))
                    initFields(l, a->getElementType(), be);
                else if (be &&
                         (a->getElementType()->isArithmeticType() ||
                             a->getElementType()->isEnumeralType()) &&
                         !isa<ImplicitValueInitExpr>(e))
                    initRule(e, a->getElementType());
            }
            return;
        }
        auto* r = t->getAsRecordDecl();
        if (!r)
            return;
        be = disc(r);
        bool hasBits = false;
        for (auto* f : r->fields())
            hasBits |= f->isBitField();
        if (be && hasBits) {
            auto location = span(i);
            if (std::find(initializers.begin(), initializers.end(), location) != initializers.end())
                return;
            initializers.push_back(location);
            std::vector<unsigned char> bytes(c.getTypeSizeInChars(t).getQuantity(), 0);
            unsigned k = 0;
            auto& layout = c.getASTRecordLayout(r);
            for (auto* f : r->fields()) {
                if (f->isUnnamedBitField())
                    continue;
                if (k >= i->getNumInits())
                    break;
                auto* e = i->getInit(k++);
                if (!f->isBitField())
                    continue;
                uint64_t v = 0;
                if (!isa<ImplicitValueInitExpr>(e)) {
                    Expr::EvalResult value;
                    if (!e->EvaluateAsInt(value, c)) {
                        error(e, "nonconstant bitfield initializer");
                        return;
                    }
                    v = value.Val.getInt().getLimitedValue();
                }
                unsigned o = layout.getFieldOffset(f->getFieldIndex()), n = f->getBitWidthValue();
                for (unsigned j = 0; j < n; j++)
                    bytes[(o + j) / 8] |= ((v >> (n - 1 - j)) & 1) << (7 - (o + j) % 8);
            }
            rule(i, [=, this] {
                std::string out = "{";
                unsigned k = 0;
                for (auto* f : r->fields()) {
                    if (f->isUnnamedBitField())
                        continue;
                    if (k >= i->getNumInits())
                        break;
                    auto* e = i->getInit(k++);
                    if (k > 1)
                        out += ",";
                    if (f->isBitField()) {
                        auto& layout = c.getASTRecordLayout(r);
                        unsigned o = layout.getFieldOffset(f->getFieldIndex()),
                                 n = f->getBitWidthValue();
                        uint64_t v = 0;
                        for (unsigned j = 0; j < n; j++)
                            v |= uint64_t((bytes[(o + j) / 8] >> ((o + j) % 8)) & 1) << j;
                        out += std::to_string(v) + "ULL";
                    } else
                        out += isa<ImplicitValueInitExpr>(e) ? "0" : expr(e);
                }
                return out + "}";
            });
            return;
        }
        unsigned k = 0;
        for (auto* f : r->fields()) {
            if (f->isUnnamedBitField())
                continue;
            if (k >= i->getNumInits())
                break;
            auto* e = i->getInit(k++);
            if (auto* l = dyn_cast<InitListExpr>(e)) {
                initFields(l, f->getType(), be);
                continue;
            }
            if (be && (f->getType()->isArithmeticType() || f->getType()->isEnumeralType()) &&
                !isa<ImplicitValueInitExpr>(e))
            {
                if (f->isBitField()) {
                    error(e, "explicit bitfield initializer requires packed lowering");
                    continue;
                }
                initRule(e, f->getType());
            }
        }
    }
    bool VisitInitListExpr(InitListExpr* i) {
        if (auto* semantic = i->getSemanticForm())
            i = semantic;
        initFields(i, i->getType());
        return true;
    }
};
struct Consumer : ASTConsumer {
    void HandleTranslationUnit(ASTContext& c) override {
        Lower l(c);
        l.TraverseDecl(c.getTranslationUnitDecl());
        if (l.failed)
            exit(2);
        llvm::outs() << l.render(0, l.text.size());
    }
};
struct Action : ASTFrontendAction {
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance&, llvm::StringRef) override {
        return std::make_unique<Consumer>();
    }
};
int main(int argc, char** argv) {
    if (argc < 2)
        return 2;
    auto b = llvm::MemoryBuffer::getFile(argv[1]);
    if (!b) {
        llvm::errs() << "cannot read input\n";
        return 2;
    }
    std::vector<std::string> args = {"-x", "c", "-std=gnu11", "-Wno-everything"};
    for (int i = 2; i < argc; i++)
        args.push_back(argv[i]);
    return !tooling::runToolOnCodeWithArgs(
        std::make_unique<Action>(), b.get()->getBuffer(), args, argv[1]);
}
