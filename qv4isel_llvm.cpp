/****************************************************************************
**
** Copyright (C) 2012 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the V4VM module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#ifdef __clang__
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wunused-parameter"
#endif // __clang__

#include <llvm/Analysis/Passes.h>
#include <llvm/Assembly/PrintModulePass.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/PassManager.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/system_error.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetData.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Linker.h>

#ifdef __clang__
#  pragma clang diagnostic pop
#endif // __clang__

#include <QtCore/QFileInfo>
#include <QtCore/QLibrary>
#include <QtCore/QStringList>
#include <QtCore/QTextStream>
#include <cstdio>
#include <iostream>

// These includes have to come last, because WTF/Platform.h defines some macros
// with very unfriendly names that collide with class fields in LLVM.
#include "qv4isel_llvm_p.h"
#include "qv4ir_p.h"

namespace QQmlJS {

void compileWithLLVM(IR::Module *module, const QString &fileName)
{
    Q_ASSERT(module);

    const QString moduleName = QFileInfo(fileName).fileName();

    LLVMInstructionSelection llvmIsel(llvm::getGlobalContext());
    llvm::Module *llvmModule = llvmIsel.getLLVMModule(module, moduleName);
    if (!llvmModule)
        return;

    // TODO: if output type is .ll, print the module to file

    llvm::PassManager PM;

    const std::string triple = llvm::sys::getDefaultTargetTriple();

    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeX86AsmParser();
    LLVMInitializeX86Disassembler();
    LLVMInitializeX86TargetMC();

    std::string err;
    const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, err);
    if (! err.empty()) {
        std::cerr << err << ", triple: " << triple << std::endl;
        assert(!"cannot create target for the host triple");
    }

    std::string cpu;
    std::string features;
    llvm::TargetOptions options;
    llvm::TargetMachine *targetMachine = target->createTargetMachine(triple, cpu, features, options, llvm::Reloc::PIC_);
    assert(targetMachine);

    llvm::TargetMachine::CodeGenFileType ft;
    QString ofName;

    ft = llvm::TargetMachine::CGFT_ObjectFile;
    ofName = fileName + QLatin1String(".o");

    // TODO:
//    ft = llvm::TargetMachine::CGFT_AssemblyFile;
//    ofName = fileName + QLatin1String(".s");

    llvm::raw_fd_ostream dest(ofName.toUtf8().constData(), err, llvm::raw_fd_ostream::F_Binary);
    llvm::formatted_raw_ostream destf(dest);
    if (!err.empty()) {
        std::cerr << err << std::endl;
        delete llvmModule;
    }

    PM.add(llvm::createScalarReplAggregatesPass());
    PM.add(llvm::createInstructionCombiningPass());
    PM.add(llvm::createGlobalOptimizerPass());
    PM.add(llvm::createFunctionInliningPass(25));
    if (targetMachine->addPassesToEmitFile(PM, destf, ft)) {
        std::cerr << err << " (probably no DataLayout in TargetMachine)" << std::endl;
    } else {
        PM.run(*llvmModule);

        destf.flush();
        dest.flush();
    }

    delete llvmModule;
}

} // QQmlJS

using namespace QQmlJS;

namespace {
QTextStream qerr(stderr, QIODevice::WriteOnly);
}

LLVMInstructionSelection::LLVMInstructionSelection(llvm::LLVMContext &context)
    : llvm::IRBuilder<>(context)
    , _llvmModule(0)
    , _llvmFunction(0)
    , _llvmValue(0)
    , _numberTy(0)
    , _valueTy(0)
    , _contextPtrTy(0)
    , _stringPtrTy(0)
    , _functionTy(0)
    , _allocaInsertPoint(0)
    , _function(0)
    , _block(0)
{
}

llvm::Module *LLVMInstructionSelection::getLLVMModule(IR::Module *module, const QString &moduleName)
{
    llvm::StringRef moduleId(moduleName.toUtf8().constData());
    llvm::Module *llvmModule = new llvm::Module(moduleId, getContext());
    qSwap(_llvmModule, llvmModule);

    _numberTy = getDoubleTy();

    std::string err;

    llvm::OwningPtr<llvm::MemoryBuffer> buffer;
    llvm::error_code ec = llvm::MemoryBuffer::getFile(llvm::StringRef("llvm_runtime.bc"), buffer);
    if (ec) {
        qWarning() << ec.message().c_str();
        assert(!"cannot load QML/JS LLVM runtime, you can generate the runtime with the command `make llvm_runtime'");
    }

    llvm::Module *llvmRuntime = llvm::getLazyBitcodeModule(buffer.get(), getContext(), &err);
    if (! err.empty()) {
        qWarning() << err.c_str();
        assert(!"cannot load QML/JS LLVM runtime");
    }

    err.clear();
    llvm::Linker::LinkModules(_llvmModule, llvmRuntime, llvm::Linker::DestroySource, &err);
    if (! err.empty()) {
        qWarning() << err.c_str();
        assert(!"cannot link the QML/JS LLVM runtime");
    }

    _valueTy = _llvmModule->getTypeByName("struct.QQmlJS::VM::Value");
    _contextPtrTy = _llvmModule->getTypeByName("struct.QQmlJS::VM::Context")->getPointerTo();
    _stringPtrTy = _llvmModule->getTypeByName("struct.QQmlJS::VM::String")->getPointerTo();

    {
        llvm::Type *args[] = { _contextPtrTy };
        _functionTy = llvm::FunctionType::get(getVoidTy(), llvm::makeArrayRef(args), false);
    }


    foreach (IR::Function *function, module->functions)
        (void) compileLLVMFunction(function);
    qSwap(_llvmModule, llvmModule);
    return llvmModule;
}

llvm::Function *LLVMInstructionSelection::getLLVMFunction(IR::Function *function)
{
    llvm::Function *&f = _functionMap[function];
    if (! f) {
        QString name = QStringLiteral("__qmljs_native_");
        if (function->name) {
            if (*function->name == QStringLiteral("%entry"))
                name = *function->name;
            else
                name += *function->name;
        }
        f = llvm::Function::Create(_functionTy, llvm::Function::ExternalLinkage, // ### make it internal
                                   qPrintable(name), _llvmModule);
    }
    return f;
}

llvm::Function *LLVMInstructionSelection::compileLLVMFunction(IR::Function *function)
{
    llvm::Function *llvmFunction = getLLVMFunction(function);

    QHash<IR::BasicBlock *, llvm::BasicBlock *> blockMap;
    QVector<llvm::Value *> tempMap;

    qSwap(_llvmFunction, llvmFunction);
    qSwap(_function, function);
    qSwap(_tempMap, tempMap);
    qSwap(_blockMap, blockMap);

    // create the LLVM blocks
    foreach (IR::BasicBlock *block, _function->basicBlocks)
        (void) getLLVMBasicBlock(block);

    // entry block
    SetInsertPoint(getLLVMBasicBlock(_function->basicBlocks.first()));

    llvm::Instruction *allocaInsertPoint = new llvm::BitCastInst(llvm::UndefValue::get(getInt32Ty()),
                                                                 getInt32Ty(), "", GetInsertBlock());
    qSwap(_allocaInsertPoint, allocaInsertPoint);

    for (int i = 0; i < _function->tempCount; ++i) {
        llvm::AllocaInst *t = newLLVMTemp(_valueTy);
        _tempMap.append(t);
    }

    foreach (llvm::Value *t, _tempMap) {
        CreateStore(llvm::Constant::getNullValue(_valueTy), t);
    }

    CreateCall(_llvmModule->getFunction("__qmljs_llvm_init_this_object"),
               _llvmFunction->arg_begin());

    foreach (IR::BasicBlock *block, _function->basicBlocks) {
        qSwap(_block, block);
        SetInsertPoint(getLLVMBasicBlock(_block));
        foreach (IR::Stmt *s, _block->statements)
            s->accept(this);
        qSwap(_block, block);
    }

    qSwap(_allocaInsertPoint, allocaInsertPoint);

    allocaInsertPoint->eraseFromParent();

    qSwap(_blockMap, blockMap);
    qSwap(_tempMap, tempMap);
    qSwap(_function, function);
    qSwap(_llvmFunction, llvmFunction);
    return llvmFunction;
}

llvm::BasicBlock *LLVMInstructionSelection::getLLVMBasicBlock(IR::BasicBlock *block)
{
    llvm::BasicBlock *&llvmBlock = _blockMap[block];
    if (! llvmBlock)
        llvmBlock = llvm::BasicBlock::Create(getContext(), llvm::Twine(),
                                             _llvmFunction);
    return llvmBlock;
}

llvm::Value *LLVMInstructionSelection::getLLVMValue(IR::Expr *expr)
{
    llvm::Value *llvmValue = 0;
    if (expr) {
        qSwap(_llvmValue, llvmValue);
        expr->accept(this);
        qSwap(_llvmValue, llvmValue);
    }
    if (! llvmValue) {
        Q_UNIMPLEMENTED();
        llvmValue = llvm::Constant::getNullValue(_valueTy);
    }
    return llvmValue;
}

llvm::Value *LLVMInstructionSelection::getLLVMTempReference(IR::Expr *expr)
{
    if (IR::Temp *t = expr->asTemp())
        return getLLVMTemp(t);

    llvm::Value *addr = newLLVMTemp(_valueTy);
    CreateStore(getLLVMValue(expr), addr);
    return addr;
}

llvm::Value *LLVMInstructionSelection::getLLVMCondition(IR::Expr *expr)
{
    llvm::Value *value = 0;
    if (IR::Temp *t = expr->asTemp()) {
        value = getLLVMTemp(t);
    } else {
        value = getLLVMValue(expr);
        if (! value) {
            Q_UNIMPLEMENTED();
            return getInt1(false);
        }

        llvm::Value *tmp = newLLVMTemp(_valueTy);
        CreateStore(value, tmp);
        value = tmp;
    }

    return CreateCall2(_llvmModule->getFunction("__qmljs_llvm_to_boolean"),
                       _llvmFunction->arg_begin(),
                       value);
}

llvm::Value *LLVMInstructionSelection::getLLVMTemp(IR::Temp *temp)
{
    if (temp->index < 0) {
        const int index = -temp->index -1;
        return CreateCall2(_llvmModule->getFunction("__qmljs_llvm_get_argument"),
                           _llvmFunction->arg_begin(), getInt32(index));
    }

    return _tempMap[temp->index];
}

llvm::Value *LLVMInstructionSelection::getStringPtr(const QString &s)
{
    llvm::Value *&value = _stringMap[s];
    if (! value) {
        const QByteArray bytes = s.toUtf8();
        value = CreateGlobalStringPtr(llvm::StringRef(bytes.constData(), bytes.size()));
        _stringMap[s] = value;
    }
    return value;
}

llvm::Value *LLVMInstructionSelection::getIdentifier(const QString &s)
{
    llvm::Value *str = getStringPtr(s);
    llvm::Value *id = CreateCall2(_llvmModule->getFunction("__qmljs_identifier_from_utf8"),
                                  _llvmFunction->arg_begin(), str);
    return id;
}

void LLVMInstructionSelection::visitExp(IR::Exp *s)
{
    getLLVMValue(s->expr);
}

void LLVMInstructionSelection::visitEnter(IR::Enter *)
{
    Q_UNREACHABLE();
}

void LLVMInstructionSelection::visitLeave(IR::Leave *)
{
    Q_UNREACHABLE();
}

void LLVMInstructionSelection::genMoveSubscript(IR::Move *s)
{
    IR::Subscript *subscript = s->target->asSubscript();
    llvm::Value *base = getLLVMTempReference(subscript->base);
    llvm::Value *index = getLLVMTempReference(subscript->index);
    llvm::Value *source = getLLVMTempReference(s->source);
    CreateCall4(_llvmModule->getFunction("__qmljs_llvm_set_element"),
                _llvmFunction->arg_begin(), base, index, source);
}

void LLVMInstructionSelection::genMoveMember(IR::Move *s)
{
    IR::Member *m = s->target->asMember();
    llvm::Value *base = getLLVMTempReference(m->base);
    llvm::Value *name = getIdentifier(*m->name);
    llvm::Value *source = getLLVMTempReference(s->source);
    CreateCall4(_llvmModule->getFunction("__qmljs_llvm_set_property"),
                _llvmFunction->arg_begin(), base, name, source);
}

void LLVMInstructionSelection::visitMove(IR::Move *s)
{
    if (s->op != IR::OpInvalid) {
        s->dump(qerr, IR::Stmt::HIR);
        qerr << endl;
        Q_UNIMPLEMENTED();
        return;
    }

    if (s->target->asSubscript()) {
        genMoveSubscript(s);
    } else if (s->target->asMember()) {
        genMoveMember(s);
    } else if (IR::Name *n = s->target->asName()) {
        llvm::Value *name = getIdentifier(*n->id);
        llvm::Value *source = getLLVMTempReference(s->source);
        CreateCall3(_llvmModule->getFunction("__qmljs_llvm_set_activation_property"),
                    _llvmFunction->arg_begin(), name, source);
    } else if (IR::Temp *t = s->target->asTemp()) {
        llvm::Value *target = getLLVMTemp(t);
        llvm::Value *source = getLLVMValue(s->source);
        CreateStore(source, target);
    } else {
        s->dump(qerr, IR::Stmt::HIR);
        qerr << endl;
        Q_UNIMPLEMENTED();
    }
}

void LLVMInstructionSelection::visitJump(IR::Jump *s)
{
    CreateBr(getLLVMBasicBlock(s->target));
}

void LLVMInstructionSelection::visitCJump(IR::CJump *s)
{
    CreateCondBr(getLLVMCondition(s->cond),
                 getLLVMBasicBlock(s->iftrue),
                 getLLVMBasicBlock(s->iffalse));
}

void LLVMInstructionSelection::visitRet(IR::Ret *s)
{
    IR::Temp *t = s->expr->asTemp();
    assert(t != 0);
    llvm::Value *result = getLLVMTemp(t);
    llvm::Value *ctx = _llvmFunction->arg_begin();
    CreateCall2(_llvmModule->getFunction("__qmljs_llvm_return"), ctx, result);
    CreateRetVoid();
}


void LLVMInstructionSelection::visitConst(IR::Const *e)
{
    llvm::Value *tmp = newLLVMTemp(_valueTy);

    switch (e->type) {
    case IR::UndefinedType:
        CreateCall(_llvmModule->getFunction("__qmljs_llvm_init_undefined"), tmp);
        break;

    case IR::NullType:
        CreateCall(_llvmModule->getFunction("__qmljs_llvm_init_null"), tmp);
        break;

    case IR::BoolType:
        CreateCall2(_llvmModule->getFunction("__qmljs_llvm_init_boolean"), tmp,
                    getInt1(e->value ? 1 : 0));
        break;

    case IR::NumberType:
        CreateCall2(_llvmModule->getFunction("__qmljs_llvm_init_number"), tmp,
                    llvm::ConstantFP::get(_numberTy, e->value));
        break;

    default:
        Q_UNREACHABLE();
    }

    _llvmValue = CreateLoad(tmp);
}

void LLVMInstructionSelection::visitString(IR::String *e)
{
    llvm::Value *tmp = newLLVMTemp(_valueTy);
    CreateCall3(_llvmModule->getFunction("__qmljs_llvm_init_string"),
                _llvmFunction->arg_begin(), tmp,
                getStringPtr(*e->value));
    _llvmValue = CreateLoad(tmp);
}

void LLVMInstructionSelection::visitName(IR::Name *e)
{
    llvm::Value *result = newLLVMTemp(_valueTy);

    if (e->id == QStringLiteral("this")) {
        CreateCall2(_llvmModule->getFunction("__qmljs_llvm_get_this_object"),
                    _llvmFunction->arg_begin(), result);
    } else {
        llvm::Value *name = getIdentifier(*e->id);
        CreateCall3(_llvmModule->getFunction("__qmljs_llvm_get_activation_property"),
                    _llvmFunction->arg_begin(), result, name);
    }
    _llvmValue = CreateLoad(result);

}

void LLVMInstructionSelection::visitTemp(IR::Temp *e)
{
    if (llvm::Value *t = getLLVMTemp(e)) {
        _llvmValue = CreateLoad(t);
    }
}

void LLVMInstructionSelection::visitClosure(IR::Closure *e)
{
    llvm::Value *tmp = newLLVMTemp(_valueTy);
    llvm::Value *clos = getLLVMFunction(e->value);
    CreateCall3(_llvmModule->getFunction("__qmljs_llvm_init_native_function"),
                _llvmFunction->arg_begin(), tmp, clos);
    _llvmValue = CreateLoad(tmp);
}

void LLVMInstructionSelection::visitUnop(IR::Unop *e)
{
    llvm::Value *result = newLLVMTemp(_valueTy);
    genUnop(result, e);
    _llvmValue = CreateLoad(result);
}

void LLVMInstructionSelection::visitBinop(IR::Binop *e)
{
    llvm::Value *result = newLLVMTemp(_valueTy);
    genBinop(result, e);
    _llvmValue = CreateLoad(result);
}

void LLVMInstructionSelection::genUnop(llvm::Value *result, IR::Unop *e)
{
    IR::Temp *t = e->expr->asTemp();
    assert(t != 0);

    llvm::Value *expr = getLLVMTemp(t);
    llvm::Value *op = 0;

    switch (e->op) {
    default:
        Q_UNREACHABLE();
        break;

    case IR::OpNot: op = _llvmModule->getFunction("__qmljs_llvm_not"); break;
    case IR::OpUMinus: op = _llvmModule->getFunction("__qmljs_llvm_uminus"); break;
    case IR::OpUPlus: op = _llvmModule->getFunction("__qmljs_llvm_uplus"); break;
    case IR::OpCompl: op = _llvmModule->getFunction("__qmljs_llvm_compl"); break;
    }

    CreateCall3(op, _llvmFunction->arg_begin(), result, expr);
}

void LLVMInstructionSelection::genBinop(llvm::Value *result, IR::Binop *e)
{
    IR::Temp *t1 = e->left->asTemp();
    IR::Temp *t2 = e->right->asTemp();
    assert(t1 != 0);
    assert(t2 != 0);

    llvm::Value *left = getLLVMTemp(t1);
    llvm::Value *right = getLLVMTemp(t2);
    llvm::Value *op = 0;
    switch (e->op) {
    case IR::OpInvalid:
    case IR::OpIfTrue:
    case IR::OpNot:
    case IR::OpUMinus:
    case IR::OpUPlus:
    case IR::OpCompl:
        Q_UNREACHABLE();
        break;

    case IR::OpBitAnd: op = _llvmModule->getFunction("__qmljs_llvm_bit_and"); break;
    case IR::OpBitOr: op = _llvmModule->getFunction("__qmljs_llvm_bit_or"); break;
    case IR::OpBitXor: op = _llvmModule->getFunction("__qmljs_llvm_bit_xor"); break;
    case IR::OpAdd: op = _llvmModule->getFunction("__qmljs_llvm_add"); break;
    case IR::OpSub: op = _llvmModule->getFunction("__qmljs_llvm_sub"); break;
    case IR::OpMul: op = _llvmModule->getFunction("__qmljs_llvm_mul"); break;
    case IR::OpDiv: op = _llvmModule->getFunction("__qmljs_llvm_div"); break;
    case IR::OpMod: op = _llvmModule->getFunction("__qmljs_llvm_mod"); break;
    case IR::OpLShift: op = _llvmModule->getFunction("__qmljs_llvm_shl"); break;
    case IR::OpRShift: op = _llvmModule->getFunction("__qmljs_llvm_shr"); break;
    case IR::OpURShift: op = _llvmModule->getFunction("__qmljs_llvm_ushr"); break;
    case IR::OpGt: op = _llvmModule->getFunction("__qmljs_llvm_gt"); break;
    case IR::OpLt: op = _llvmModule->getFunction("__qmljs_llvm_lt"); break;
    case IR::OpGe: op = _llvmModule->getFunction("__qmljs_llvm_ge"); break;
    case IR::OpLe: op = _llvmModule->getFunction("__qmljs_llvm_le"); break;
    case IR::OpEqual: op = _llvmModule->getFunction("__qmljs_llvm_eq"); break;
    case IR::OpNotEqual: op = _llvmModule->getFunction("__qmljs_llvm_ne"); break;
    case IR::OpStrictEqual: op = _llvmModule->getFunction("__qmljs_llvm_se"); break;
    case IR::OpStrictNotEqual: op = _llvmModule->getFunction("__qmljs_llvm_sne"); break;
    case IR::OpInstanceof: op = _llvmModule->getFunction("__qmljs_llvm_instanceof"); break;
    case IR::OpIn: op = _llvmModule->getFunction("__qmljs_llvm_in"); break;

    case IR::OpAnd:
    case IR::OpOr:
        Q_UNREACHABLE();
        break;
    }

    CreateCall4(op, _llvmFunction->arg_begin(), result, left, right);
}

llvm::AllocaInst *LLVMInstructionSelection::newLLVMTemp(llvm::Type *type, llvm::Value *size)
{
    llvm::AllocaInst *addr = new llvm::AllocaInst(type, size, llvm::Twine(), _allocaInsertPoint);
    return addr;
}

llvm::Value * LLVMInstructionSelection::genArguments(IR::ExprList *exprs, int &argc)
{
    llvm::Value *args = 0;

    argc = 0;
    for (IR::ExprList *it = exprs; it; it = it->next)
        ++argc;

    if (argc)
        args = newLLVMTemp(_valueTy, getInt32(argc));
    else
        args = llvm::Constant::getNullValue(_valueTy->getPointerTo());

    int i = 0;
    for (IR::ExprList *it = exprs; it; it = it->next) {
        llvm::Value *arg = getLLVMValue(it->expr);
        CreateStore(arg, CreateConstGEP1_32(args, i++));
    }

    return args;
}

void LLVMInstructionSelection::genCallMember(IR::Call *e, llvm::Value *result)
{
    if (! result)
        result = newLLVMTemp(_valueTy);

    IR::Member *m = e->base->asMember();
    llvm::Value *thisObject = getLLVMTemp(m->base->asTemp());
    llvm::Value *name = getIdentifier(*m->name);

    int argc = 0;
    llvm::Value *args = genArguments(e->args, argc);

    llvm::Value *actuals[] = {
        _llvmFunction->arg_begin(),
        result,
        thisObject,
        name,
        args,
        getInt32(argc)
    };

    CreateCall(_llvmModule->getFunction("__qmljs_llvm_call_property"), llvm::ArrayRef<llvm::Value *>(actuals));
    _llvmValue = CreateLoad(result);
}

void LLVMInstructionSelection::genConstructMember(IR::New *e, llvm::Value *result)
{
    if (! result)
        result = newLLVMTemp(_valueTy);

    IR::Member *m = e->base->asMember();
    llvm::Value *thisObject = getLLVMTemp(m->base->asTemp());
    llvm::Value *name = getIdentifier(*m->name);

    int argc = 0;
    llvm::Value *args = genArguments(e->args, argc);

    llvm::Value *actuals[] = {
        _llvmFunction->arg_begin(),
        result,
        thisObject,
        name,
        args,
        getInt32(argc)
    };

    CreateCall(_llvmModule->getFunction("__qmljs_llvm_construct_property"), llvm::ArrayRef<llvm::Value *>(actuals));
    _llvmValue = CreateLoad(result);
}

void LLVMInstructionSelection::genCallTemp(IR::Call *e, llvm::Value *result)
{
    if (! result)
        result = newLLVMTemp(_valueTy);

    llvm::Value *func = getLLVMTempReference(e->base);

    int argc = 0;
    llvm::Value *args = genArguments(e->args, argc);

    llvm::Value *thisObject = llvm::Constant::getNullValue(_valueTy->getPointerTo());

    llvm::Value *actuals[] = {
        _llvmFunction->arg_begin(),
        result,
        thisObject,
        func,
        args,
        getInt32(argc)
    };

    CreateCall(_llvmModule->getFunction("__qmljs_llvm_call_value"), actuals);

    _llvmValue = CreateLoad(result);
}

void LLVMInstructionSelection::genConstructTemp(IR::New *e, llvm::Value *result)
{
    if (! result)
        result = newLLVMTemp(_valueTy);

    llvm::Value *func = getLLVMTempReference(e->base);

    int argc = 0;
    llvm::Value *args = genArguments(e->args, argc);

    llvm::Value *actuals[] = {
        _llvmFunction->arg_begin(),
        result,
        func,
        args,
        getInt32(argc)
    };

    CreateCall(_llvmModule->getFunction("__qmljs_llvm_construct_value"), actuals);

    _llvmValue = CreateLoad(result);
}

void LLVMInstructionSelection::genCallName(IR::Call *e, llvm::Value *result)
{
    IR::Name *base = e->base->asName();

    if (! result)
        result = newLLVMTemp(_valueTy);

    if (! base->id) {
        switch (base->builtin) {
        case IR::Name::builtin_invalid:
            break;

        case IR::Name::builtin_typeof:
            // inline void __qmljs_typeof(Context *ctx, Value *result, const Value *value)
            CreateCall3(_llvmModule->getFunction("__qmljs_llvm_typeof"),
                        _llvmFunction->arg_begin(), result, getLLVMTempReference(e->args->expr));
            _llvmValue = CreateLoad(result);
            return;

        case IR::Name::builtin_delete: {
            if (IR::Subscript *subscript = e->args->expr->asSubscript()) {
                CreateCall4(_llvmModule->getFunction("__qmljs_llvm_delete_subscript"),
                           _llvmFunction->arg_begin(),
                           result,
                           getLLVMTempReference(subscript->base),
                           getLLVMTempReference(subscript->index));
                _llvmValue = CreateLoad(result);
                return;
            } else if (IR::Member *member = e->args->expr->asMember()) {
                CreateCall4(_llvmModule->getFunction("__qmljs_llvm_delete_member"),
                           _llvmFunction->arg_begin(),
                           result,
                           getLLVMTempReference(member->base),
                           getIdentifier(*member->name));
                _llvmValue = CreateLoad(result);
                return;
            } else if (IR::Name *name = e->args->expr->asName()) {
                CreateCall3(_llvmModule->getFunction("__qmljs_llvm_delete_property"),
                           _llvmFunction->arg_begin(),
                           result,
                           getIdentifier(*name->id));
                _llvmValue = CreateLoad(result);
                return;
            } else {
                CreateCall3(_llvmModule->getFunction("__qmljs_llvm_delete_value"),
                           _llvmFunction->arg_begin(),
                           result,
                           getLLVMTempReference(e->args->expr));
                _llvmValue = CreateLoad(result);
                return;
            }
        }   break;

        case IR::Name::builtin_throw:
            CreateCall2(_llvmModule->getFunction("__qmljs_llvm_throw"),
                        _llvmFunction->arg_begin(), getLLVMTempReference(e->args->expr));
            _llvmValue = llvm::UndefValue::get(_valueTy);
            return;

        case IR::Name::builtin_rethrow:
            CreateCall2(_llvmModule->getFunction("__qmljs_llvm_rethrow"),
                        _llvmFunction->arg_begin(), result);
            _llvmValue = CreateLoad(result);
            return;
        }

        Q_UNREACHABLE();
    } else {
        llvm::Value *name = getIdentifier(*base->id);

        int argc = 0;
        llvm::Value *args = genArguments(e->args, argc);

        CreateCall5(_llvmModule->getFunction("__qmljs_llvm_call_activation_property"),
                    _llvmFunction->arg_begin(), result, name, args, getInt32(argc));

        _llvmValue = CreateLoad(result);
    }
}

void LLVMInstructionSelection::genConstructName(IR::New *e, llvm::Value *result)
{
    IR::Name *base = e->base->asName();

    if (! result)
        result = newLLVMTemp(_valueTy);

    if (! base->id) {
        Q_UNREACHABLE();
    } else {
        llvm::Value *name = getIdentifier(*base->id);

        int argc = 0;
        llvm::Value *args = genArguments(e->args, argc);

        CreateCall5(_llvmModule->getFunction("__qmljs_construct_activation_property"),
                    _llvmFunction->arg_begin(), result, name, args, getInt32(argc));

        _llvmValue = CreateLoad(result);
    }
}

void LLVMInstructionSelection::visitCall(IR::Call *e)
{
    if (e->base->asMember()) {
        genCallMember(e);
    } else if (e->base->asTemp()) {
        genCallTemp(e);
    } else if (e->base->asName()) {
        genCallName(e);
    } else if (IR::Temp *t = e->base->asTemp()) {
        llvm::Value *base = getLLVMTemp(t);

        int argc = 0;
        llvm::Value *args = genArguments(e->args, argc);

        llvm::Value *result = newLLVMTemp(_valueTy);
        CreateStore(llvm::Constant::getNullValue(_valueTy), result);
        CreateCall5(_llvmModule->getFunction("__qmljs_llvm_call_value"),
                    _llvmFunction->arg_begin(), result, base, args, getInt32(argc));
        _llvmValue = CreateLoad(result);
    } else {
        Q_UNIMPLEMENTED();
    }
}

void LLVMInstructionSelection::visitNew(IR::New *e)
{
    if (e->base->asMember()) {
        genConstructMember(e);
    } else if (e->base->asTemp()) {
        genConstructTemp(e);
    } else if (e->base->asName()) {
        genConstructName(e);
    } else if (IR::Temp *t = e->base->asTemp()) {
        llvm::Value *base = getLLVMTemp(t);

        int argc = 0;
        llvm::Value *args = genArguments(e->args, argc);

        llvm::Value *result = newLLVMTemp(_valueTy);
        CreateStore(llvm::Constant::getNullValue(_valueTy), result);
        CreateCall5(_llvmModule->getFunction("__qmljs_llvm_construct_value"),
                    _llvmFunction->arg_begin(), result, base, args, getInt32(argc));
        _llvmValue = CreateLoad(result);
    } else {
        Q_UNIMPLEMENTED();
    }
}

void LLVMInstructionSelection::visitSubscript(IR::Subscript *e)
{
    llvm::Value *result = newLLVMTemp(_valueTy);
    llvm::Value *base = getLLVMTempReference(e->base);
    llvm::Value *index = getLLVMTempReference(e->index);
    CreateCall4(_llvmModule->getFunction("__qmljs_llvm_get_element"),
                _llvmFunction->arg_begin(), result, base, index);
    _llvmValue = CreateLoad(result);
}

void LLVMInstructionSelection::visitMember(IR::Member *e)
{
    llvm::Value *result = newLLVMTemp(_valueTy);
    llvm::Value *base = getLLVMTempReference(e->base);
    llvm::Value *name = getIdentifier(*e->name);

    CreateCall4(_llvmModule->getFunction("__qmljs_llvm_get_property"),
                _llvmFunction->arg_begin(), result, base, name);
    _llvmValue = CreateLoad(result);
}
