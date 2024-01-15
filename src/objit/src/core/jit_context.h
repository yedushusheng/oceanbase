/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef JIT_CONTEXT_H
#define JIT_CONTEXT_H
#include "core/ob_orc_jit.h"
#include "expr/ob_llvm_type.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"

/** Note:编译执行比解释执行的优势毋庸赘言,OceanBase也是采用把存储过程编译为机器码的方式进行执行,
 * 但是和Oracle实现不同的是,OceanBase采用JIT技术,无需依赖外部C编译器和生成动态链接库.
 * 采用这一技术方案,有如下优点:
 * 无需研究体系结构相关的复杂的机器码生成,就可以获得编译执行的效果;
 * 生成LLVM IR比生成汇编简单很多,且自然拥有了考虑跨平台能力;
 * LLVM提供的成熟的优化模块都是基于LLVM IR的,clang等项目中对优化器的改进都可以直接被我们受益;
 * 用JIT技术在Observer内直接控制生成编译后的代码,比Oracle采用的在数据库外部编译器编译结合动态链接库装载的方式在可控性,性能,可维护性各方面都有优势;
 * PL的编译执行可以和SQL表达式及部分physical operator甚至是整个查询计划采用编译执行相结合,获取整体性能的提升.
*/
namespace oceanbase
{
namespace jit
{
namespace core
{
struct JitContext
{
public:
  explicit JitContext()
      : Compile(false), TheContext(nullptr), TheJIT(nullptr)
  {
  }

  int InitializeModule(ObOrcJit &jit);
  void compile();
  int optimize();

  ObLLVMContext& get_context() { return *TheContext; }
  IRBuilder<>& get_builder() { return *Builder; }
  Module& get_module() { return *TheModule; }
  ObOrcJit* get_jit() { return TheJIT; }

public:
  bool Compile;
  
  ObLLVMContext *TheContext;
  std::unique_ptr<IRBuilder<>> Builder;
  std::unique_ptr<Module> TheModule;
  ObOrcJit *TheJIT;
  std::unique_ptr<legacy::FunctionPassManager> TheFPM;
};

class StringMemoryBuffer : public llvm::MemoryBuffer
{
public:
  StringMemoryBuffer(const char *debug_buffer, int64_t debug_length) {
    init(debug_buffer, debug_buffer + debug_length, false);
  }
  BufferKind getBufferKind() const override { return MemoryBuffer_Malloc; }
};

class ObDWARFContext
{
public:
  ObDWARFContext(char* DebugBuf, int64_t DebugLen)
    : MemoryBuf(DebugBuf, DebugLen), MemoryRef(MemoryBuf) {}

  ~ObDWARFContext() {}

  int init();

public:
  StringMemoryBuffer MemoryBuf;
  llvm::MemoryBufferRef MemoryRef;
  std::unique_ptr<llvm::object::Binary> Bin;

  std::unique_ptr<llvm::DWARFContext> Context;
};

} // core
} // jit
} // oceanbase

#endif /* JIT_CONTEXT_H */
