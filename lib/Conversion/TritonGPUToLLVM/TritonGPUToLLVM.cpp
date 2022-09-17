#include "triton/Conversion/TritonGPUToLLVM/TritonGPUToLLVM.h"
#include "../PassDetail.h"
#include "mlir/Analysis/SliceAnalysis.h"
#include "mlir/Conversion/ArithmeticToLLVM/ArithmeticToLLVM.h"
#include "mlir/Conversion/GPUToNVVM/GPUToNVVMPass.h"
#include "mlir/Conversion/LLVMCommon/LoweringOptions.h"
#include "mlir/Conversion/LLVMCommon/Pattern.h"
#include "mlir/Conversion/MathToLLVM/MathToLLVM.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/GPU/GPUDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Transforms/DialectConversion.h"
#include "triton/Analysis/Allocation.h"
#include "triton/Analysis/AxisInfo.h"
#include "triton/Analysis/Utility.h"
#include "triton/Conversion/MLIRTypes.h"
#include "triton/Conversion/TritonGPUToLLVM/PtxAsmFormat.h"
#include "triton/Conversion/TritonToTritonGPU/TritonToTritonGPU.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "triton/Dialect/TritonGPU/IR/Dialect.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include <memory>
#include <numeric>
#include <string>

using namespace mlir;
using namespace mlir::triton;
using ::mlir::triton::gpu::BlockedEncodingAttr;
using ::mlir::triton::gpu::getElemsPerThread;
using ::mlir::triton::gpu::getShapePerCTA;
using ::mlir::triton::gpu::MmaEncodingAttr;
using ::mlir::triton::gpu::SharedEncodingAttr;
using ::mlir::triton::gpu::SliceEncodingAttr;

namespace mlir {
namespace LLVM {

static StringRef getStructAttrsAttrName() { return "llvm.struct_attrs"; }

} // namespace LLVM
} // namespace mlir

namespace {

namespace type = mlir::triton::type;

class TritonGPUToLLVMTypeConverter;

// FuncOpConversion/FuncOpConversionBase is borrowed from
// https://github.com/llvm/llvm-project/blob/fae656b2dd80246c3c6f01e9c77c49560368752c/mlir/lib/Conversion/FuncToLLVM/FuncToLLVM.cpp#L276
// since it is not exposed on header files in mlir v14
// TODO(Superjomn) Remove the code when mlir v15.0 is included.
// All the rights are reserved by LLVM community.

/// Only retain those attributes that are not constructed by
/// `LLVMFuncOp::build`. If `filterArgAttrs` is set, also filter out argument
/// attributes.
static void filterFuncAttributes(ArrayRef<NamedAttribute> attrs,
                                 bool filterArgAttrs,
                                 SmallVectorImpl<NamedAttribute> &result) {
  for (const auto &attr : attrs) {
    if (attr.getName() == SymbolTable::getSymbolAttrName() ||
        attr.getName() == FunctionOpInterface::getTypeAttrName() ||
        attr.getName() == "std.varargs" ||
        (filterArgAttrs &&
         attr.getName() == FunctionOpInterface::getArgDictAttrName()))
      continue;
    result.push_back(attr);
  }
}

/// Helper function for wrapping all attributes into a single DictionaryAttr
static auto wrapAsStructAttrs(OpBuilder &b, ArrayAttr attrs) {
  return DictionaryAttr::get(
      b.getContext(), b.getNamedAttr(LLVM::getStructAttrsAttrName(), attrs));
}

struct FuncOpConversionBase : public ConvertOpToLLVMPattern<FuncOp> {
protected:
  using ConvertOpToLLVMPattern<FuncOp>::ConvertOpToLLVMPattern;

  // Convert input FuncOp to LLVMFuncOp by using the LLVMTypeConverter provided
  // to this legalization pattern.
  LLVM::LLVMFuncOp
  convertFuncOpToLLVMFuncOp(FuncOp funcOp,
                            ConversionPatternRewriter &rewriter) const {
    // Convert the original function arguments. They are converted using the
    // LLVMTypeConverter provided to this legalization pattern.
    auto varargsAttr = funcOp->getAttrOfType<BoolAttr>("func.varargs");
    TypeConverter::SignatureConversion result(funcOp.getNumArguments());
    auto llvmType = getTypeConverter()->convertFunctionSignature(
        funcOp.getType(), varargsAttr && varargsAttr.getValue(), result);
    if (!llvmType)
      return nullptr;

    // Propagate argument/result attributes to all converted arguments/result
    // obtained after converting a given original argument/result.
    SmallVector<NamedAttribute, 4> attributes;
    filterFuncAttributes(funcOp->getAttrs(), /*filterArgAndResAttrs=*/true,
                         attributes);
    if (ArrayAttr resAttrDicts = funcOp.getAllResultAttrs()) {
      assert(!resAttrDicts.empty() && "expected array to be non-empty");
      auto newResAttrDicts =
          (funcOp.getNumResults() == 1)
              ? resAttrDicts
              : rewriter.getArrayAttr(
                    {wrapAsStructAttrs(rewriter, resAttrDicts)});
      attributes.push_back(rewriter.getNamedAttr(
          FunctionOpInterface::getResultDictAttrName(), newResAttrDicts));
    }
    if (ArrayAttr argAttrDicts = funcOp.getAllArgAttrs()) {
      SmallVector<Attribute, 4> newArgAttrs(
          llvmType.cast<LLVM::LLVMFunctionType>().getNumParams());
      for (unsigned i = 0, e = funcOp.getNumArguments(); i < e; ++i) {
        auto mapping = result.getInputMapping(i);
        assert(mapping && "unexpected deletion of function argument");
        for (size_t j = 0; j < mapping->size; ++j)
          newArgAttrs[mapping->inputNo + j] = argAttrDicts[i];
      }
      attributes.push_back(
          rewriter.getNamedAttr(FunctionOpInterface::getArgDictAttrName(),
                                rewriter.getArrayAttr(newArgAttrs)));
    }
    for (const auto &pair : llvm::enumerate(attributes)) {
      if (pair.value().getName() == "llvm.linkage") {
        attributes.erase(attributes.begin() + pair.index());
        break;
      }
    }

    // Create an LLVM function, use external linkage by default until MLIR
    // functions have linkage.
    LLVM::Linkage linkage = LLVM::Linkage::External;
    if (funcOp->hasAttr("llvm.linkage")) {
      auto attr =
          funcOp->getAttr("llvm.linkage").dyn_cast<mlir::LLVM::LinkageAttr>();
      if (!attr) {
        funcOp->emitError()
            << "Contains llvm.linkage attribute not of type LLVM::LinkageAttr";
        return nullptr;
      }
      linkage = attr.getLinkage();
    }
    auto newFuncOp = rewriter.create<LLVM::LLVMFuncOp>(
        funcOp.getLoc(), funcOp.getName(), llvmType, linkage,
        /*dsoLocal*/ false, attributes);
    rewriter.inlineRegionBefore(funcOp.getBody(), newFuncOp.getBody(),
                                newFuncOp.end());
    if (failed(rewriter.convertRegionTypes(&newFuncOp.getBody(), *typeConverter,
                                           &result)))
      return nullptr;

    return newFuncOp;
  }
};

/// FuncOp legalization pattern that converts MemRef arguments to pointers to
/// MemRef descriptors (LLVM struct data types) containing all the MemRef type
/// information.
static constexpr StringRef kEmitIfaceAttrName = "llvm.emit_c_interface";
struct FuncOpConversion : public FuncOpConversionBase {
  FuncOpConversion(LLVMTypeConverter &converter, int numWarps,
                   PatternBenefit benefit)
      : FuncOpConversionBase(converter, benefit), NumWarps(numWarps) {}

  LogicalResult
  matchAndRewrite(FuncOp funcOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto newFuncOp = convertFuncOpToLLVMFuncOp(funcOp, rewriter);
    if (!newFuncOp)
      return failure();

    auto ctx = funcOp->getContext();

    // Set an attribute to indicate this function is a kernel entry.
    newFuncOp->setAttr(NVVMMetadataField::Kernel,
                       rewriter.getIntegerAttr(type::u1Ty(ctx), 1));

    // Set an attribute for maxntidx, it could be used in latter LLVM codegen
    // for `nvvm.annotation` metadata.
    newFuncOp->setAttr(
        NVVMMetadataField::MaxNTid,
        rewriter.getIntegerAttr(type::i32Ty(ctx), 32 * NumWarps));

    rewriter.eraseOp(funcOp);
    return success();
  }

private:
  int NumWarps{0};
};

struct ReturnOpConversion : public ConvertOpToLLVMPattern<::mlir::ReturnOp> {
  using ConvertOpToLLVMPattern<ReturnOp>::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(ReturnOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    unsigned numArguments = op.getNumOperands();

    // Currently, Triton kernel function always return nothing.
    // TODO(Superjomn) add support for non-inline device function
    if (numArguments > 0) {
      return rewriter.notifyMatchFailure(
          op, "Only kernel function with nothing returned is supported.");
    }

    rewriter.replaceOpWithNewOp<LLVM::ReturnOp>(op, TypeRange(), ValueRange(),
                                                op->getAttrs());
    return success();
  }
};

static Value createIndexAttrConstant(OpBuilder &builder, Location loc,
                                     Type resultType, int64_t value) {
  return builder.create<LLVM::ConstantOp>(
      loc, resultType, builder.getIntegerAttr(builder.getIndexType(), value));
}

static Value createLLVMIntegerConstant(OpBuilder &builder, Location loc,
                                       LLVMTypeConverter *converter, Type ty,
                                       int64_t value) {
  return builder.create<LLVM::ConstantOp>(loc, converter->convertType(ty),
                                          builder.getIntegerAttr(ty, value));
}

Value getStructFromElements(Location loc, ValueRange resultVals,
                            ConversionPatternRewriter &rewriter,
                            Type structType) {
  Value llvmStruct = rewriter.create<LLVM::UndefOp>(loc, structType);
  for (auto v : llvm::enumerate(resultVals)) {
    llvmStruct = rewriter.create<LLVM::InsertValueOp>(
        loc, structType, llvmStruct, v.value(),
        rewriter.getI64ArrayAttr(v.index()));
  }
  return llvmStruct;
}

template <typename T>
static SmallVector<T> getMultiDimIndex(T linear_index, ArrayRef<T> shape) {
  // sizes {a, b, c, d}  ->  acc_mul {b*c*d, c*d, d, 1}
  size_t rank = shape.size();
  T acc_mul = 1;
  for (size_t i = 1; i < rank; ++i) {
    acc_mul *= shape[i];
  }
  T linear_remain = linear_index;
  SmallVector<T> multidim_index(rank);
  for (size_t i = 0; i < rank; ++i) {
    multidim_index[i] = linear_remain / acc_mul;
    linear_remain = linear_remain % acc_mul;
    if (i != (rank - 1)) {
      acc_mul = acc_mul / shape[i + 1];
    }
  }
  return multidim_index;
}

template <typename T>
static T getLinearIndex(ArrayRef<T> multidim_index, ArrayRef<T> shape) {
  assert(multidim_index.size() == shape.size());
  // sizes {a, b, c, d}  ->  acc_mul {b*c*d, c*d, d, 1}
  size_t rank = shape.size();
  T acc_mul = 1;
  for (size_t i = 1; i < rank; ++i) {
    acc_mul *= shape[i];
  }
  T linear_index = 0;
  for (size_t i = 0; i < rank; ++i) {
    linear_index += multidim_index[i] * acc_mul;
    if (i != (rank - 1)) {
      acc_mul = acc_mul / shape[i + 1];
    }
  }
  return linear_index;
}

struct ConvertTritonGPUOpToLLVMPatternBase {
  static SmallVector<Value>
  getElementsFromStruct(Location loc, Value llvmStruct, unsigned elems,
                        ConversionPatternRewriter &rewriter) {
    SmallVector<Value> results(elems);
    for (unsigned i = 0; i < elems; ++i) {
      Type type =
          llvmStruct.getType().cast<LLVM::LLVMStructType>().getBody()[i];
      results[i] = rewriter.create<LLVM::ExtractValueOp>(
          loc, type, llvmStruct, rewriter.getI64ArrayAttr(i));
    }
    return results;
  }
};

template <typename SourceOp>
class ConvertTritonGPUOpToLLVMPattern
    : public ConvertOpToLLVMPattern<SourceOp>,
      public ConvertTritonGPUOpToLLVMPatternBase {
public:
  using OpAdaptor = typename SourceOp::Adaptor;

  explicit ConvertTritonGPUOpToLLVMPattern(LLVMTypeConverter &typeConverter,
                                           PatternBenefit benefit = 1)
      : ConvertOpToLLVMPattern<SourceOp>(typeConverter, benefit) {}

  SmallVector<Value> delinearize(ConversionPatternRewriter &rewriter,
                                 Location loc, Value linear,
                                 ArrayRef<unsigned> shape,
                                 ArrayRef<unsigned> order) const {
    unsigned rank = shape.size();
    assert(rank == order.size());
    SmallVector<unsigned> reordered(rank);
    for (unsigned i = 0; i < rank; ++i) {
      reordered[i] = shape[order[i]];
    }
    auto reorderedMultiDim = delinearize(rewriter, loc, linear, reordered);
    SmallVector<Value> multiDim(rank);
    for (unsigned i = 0; i < rank; ++i) {
      multiDim[order[i]] = reorderedMultiDim[i];
    }
    return multiDim;
  }

  SmallVector<Value> delinearize(ConversionPatternRewriter &rewriter,
                                 Location loc, Value linear,
                                 ArrayRef<unsigned> shape) const {
    unsigned rank = shape.size();
    assert(rank > 0);
    SmallVector<Value> multiDim(rank);
    if (rank == 1) {
      multiDim[0] = linear;
    } else {
      Value remained = linear;
      for (auto &&en : llvm::enumerate(llvm::reverse(shape.drop_front()))) {
        Value dimSize = createIndexAttrConstant(
            rewriter, loc, this->getTypeConverter()->getIndexType(),
            en.value());
        multiDim[rank - 1 - en.index()] =
            rewriter.create<LLVM::URemOp>(loc, remained, dimSize);
        remained = rewriter.create<LLVM::UDivOp>(loc, remained, dimSize);
      }
      multiDim[0] = remained;
    }
    return multiDim;
  }

  Value linearize(ConversionPatternRewriter &rewriter, Location loc,
                  ArrayRef<Value> multiDim, ArrayRef<unsigned> shape) const {
    int rank = multiDim.size();
    Value linear = createIndexAttrConstant(
        rewriter, loc, this->getTypeConverter()->getIndexType(), 0);
    if (rank > 0) {
      linear = multiDim.front();
      for (auto &&z : llvm::zip(multiDim.drop_front(), shape.drop_front())) {
        Value dimSize = createIndexAttrConstant(
            rewriter, loc, this->getTypeConverter()->getIndexType(),
            std::get<1>(z));
        linear = rewriter.create<LLVM::AddOp>(
            loc, rewriter.create<LLVM::MulOp>(loc, linear, dimSize),
            std::get<0>(z));
      }
    }
    return linear;
  }

  SmallVector<Value>
  emitBaseIndexForBlockedLayout(Location loc, ConversionPatternRewriter &b,
                                const BlockedEncodingAttr &blocked_layout,
                                ArrayRef<int64_t> shape) const {
    auto llvmIndexTy = this->getTypeConverter()->getIndexType();
    auto cast = b.create<UnrealizedConversionCastOp>(
        loc, TypeRange{llvmIndexTy},
        ValueRange{b.create<::mlir::gpu::ThreadIdOp>(
            loc, b.getIndexType(), ::mlir::gpu::Dimension::x)});
    Value threadId = cast.getResult(0);
    Value warpSize = createIndexAttrConstant(b, loc, llvmIndexTy, 32);
    Value laneId = b.create<LLVM::URemOp>(loc, threadId, warpSize);
    Value warpId = b.create<LLVM::UDivOp>(loc, threadId, warpSize);
    auto sizePerThread = blocked_layout.getSizePerThread();
    auto threadsPerWarp = blocked_layout.getThreadsPerWarp();
    auto warpsPerCTA = blocked_layout.getWarpsPerCTA();
    auto order = blocked_layout.getOrder();
    unsigned rank = shape.size();

    // step 1, delinearize threadId to get the base index
    SmallVector<Value> multiDimWarpId =
        delinearize(b, loc, warpId, warpsPerCTA, order);
    SmallVector<Value> multiDimThreadId =
        delinearize(b, loc, laneId, threadsPerWarp, order);
    SmallVector<Value> multiDimBase(rank);
    for (unsigned k = 0; k < rank; ++k) {
      // Wrap around multiDimWarpId/multiDimThreadId incase
      // shape[k] > shapePerCTA[k]
      unsigned maxWarps =
          ceil<unsigned>(shape[k], sizePerThread[k] * threadsPerWarp[k]);
      unsigned maxThreads = ceil<unsigned>(shape[k], sizePerThread[k]);
      multiDimWarpId[k] = b.create<LLVM::URemOp>(
          loc, multiDimWarpId[k],
          createIndexAttrConstant(b, loc, llvmIndexTy, maxWarps));
      multiDimThreadId[k] = b.create<LLVM::URemOp>(
          loc, multiDimThreadId[k],
          createIndexAttrConstant(b, loc, llvmIndexTy, maxThreads));
      // multiDimBase[k] = (multiDimThreadId[k] +
      //                    multiDimWarpId[k] * threadsPerWarp[k]) *
      //                   sizePerThread[k];
      Value threadsPerWarpK =
          createIndexAttrConstant(b, loc, llvmIndexTy, threadsPerWarp[k]);
      Value sizePerThreadK =
          createIndexAttrConstant(b, loc, llvmIndexTy, sizePerThread[k]);
      multiDimBase[k] = b.create<LLVM::MulOp>(
          loc, sizePerThreadK,
          b.create<LLVM::AddOp>(
              loc, multiDimThreadId[k],
              b.create<LLVM::MulOp>(loc, multiDimWarpId[k], threadsPerWarpK)));
    }
    return multiDimBase;
  }

  SmallVector<SmallVector<Value>> emitIndices(Location loc,
                                              ConversionPatternRewriter &b,
                                              const Attribute &layout,
                                              ArrayRef<int64_t> shape) const {
    if (auto blocked = layout.dyn_cast<BlockedEncodingAttr>()) {
      return emitIndicesForBlockedLayout(loc, b, blocked, shape);
    } else if (auto slice = layout.dyn_cast<SliceEncodingAttr>()) {
      return emitIndicesForSliceLayout(loc, b, slice, shape);
    } else {
      assert(0 && "emitIndices for layouts other than blocked & slice not "
                  "implemented yet");
      return {};
    }
  }

  SmallVector<SmallVector<Value>>
  emitIndicesForSliceLayout(Location loc, ConversionPatternRewriter &b,
                            const SliceEncodingAttr &sliceLayout,
                            ArrayRef<int64_t> shape) const {
    auto parent = sliceLayout.getParent();
    unsigned dim = sliceLayout.getDim();
    size_t rank = shape.size();
    if (auto blockedParent = parent.dyn_cast<BlockedEncodingAttr>()) {
      SmallVector<int64_t> paddedShape(rank + 1);
      for (unsigned d = 0; d < rank + 1; ++d) {
        if (d < dim) {
          paddedShape[d] = shape[d];
        } else if (d == dim) {
          paddedShape[d] = 1;
        } else {
          paddedShape[d] = shape[d - 1];
        }
      }
      auto paddedIndices =
          emitIndicesForBlockedLayout(loc, b, blockedParent, paddedShape);
      unsigned numIndices = paddedIndices.size();
      SmallVector<SmallVector<Value>> resultIndices(numIndices);
      for (unsigned i = 0; i < numIndices; ++i) {
        for (unsigned d = 0; d < rank + 1; ++d) {
          if (d != dim) {
            resultIndices[i].push_back(paddedIndices[i][d]);
          }
        }
      }
      return resultIndices;

    } else if (auto sliceParent = parent.dyn_cast<SliceEncodingAttr>()) {
      assert(0 && "emitIndicesForSliceLayout with parent of sliceLayout"
                  "is not implemented yet");
      return {};

    } else {
      assert(0 && "emitIndicesForSliceLayout with parent other than blocked & "
                  "slice not implemented yet");
      return {};
    }
  }

  // Emit indices calculation within each ConversionPattern
  // TODO: [goostavz] Double confirm the redundant indices calculations will
  //       be eliminated in the consequent MLIR/LLVM optimization. We might
  //       implement a indiceCache if necessary.
  SmallVector<SmallVector<Value>>
  emitIndicesForBlockedLayout(Location loc, ConversionPatternRewriter &b,
                              const BlockedEncodingAttr &blockedLayout,
                              ArrayRef<int64_t> shape) const {
    auto llvmIndexTy = this->getTypeConverter()->getIndexType();
    auto sizePerThread = blockedLayout.getSizePerThread();
    auto threadsPerWarp = blockedLayout.getThreadsPerWarp();
    auto warpsPerCTA = blockedLayout.getWarpsPerCTA();
    unsigned rank = shape.size();
    SmallVector<unsigned> shapePerCTA(rank);
    for (unsigned k = 0; k < rank; ++k) {
      shapePerCTA[k] = sizePerThread[k] * threadsPerWarp[k] * warpsPerCTA[k];
    }

    // step 1, delinearize threadId to get the base index
    auto multiDimBase =
        emitBaseIndexForBlockedLayout(loc, b, blockedLayout, shape);

    // step 2, get offset of each element
    unsigned elemsPerThread = 1;
    SmallVector<SmallVector<unsigned>> offset(rank);
    SmallVector<unsigned> multiDimElemsPerThread(rank);
    for (unsigned k = 0; k < rank; ++k) {
      multiDimElemsPerThread[k] =
          ceil<unsigned>(shape[k], shapePerCTA[k]) * sizePerThread[k];
      elemsPerThread *= multiDimElemsPerThread[k];
      // 1 block in minimum if shape[k] is less than shapePerCTA[k]
      for (unsigned blockOffset = 0;
           blockOffset < ceil<unsigned>(shape[k], shapePerCTA[k]);
           ++blockOffset)
        for (unsigned warpOffset = 0; warpOffset < warpsPerCTA[k]; ++warpOffset)
          for (unsigned threadOffset = 0; threadOffset < threadsPerWarp[k];
               ++threadOffset)
            for (unsigned elemOffset = 0; elemOffset < sizePerThread[k];
                 ++elemOffset)
              offset[k].push_back(blockOffset * sizePerThread[k] *
                                      threadsPerWarp[k] * warpsPerCTA[k] +
                                  warpOffset * sizePerThread[k] *
                                      threadsPerWarp[k] +
                                  threadOffset * sizePerThread[k] + elemOffset);
    }
    // step 3, add offset to base, and reorder the sequence of indices,
    //         to guarantee that elems in a same sizePerThread are adjacent in
    //         order
    SmallVector<SmallVector<Value>> multiDimIdx(elemsPerThread);
    unsigned accumSizePerThread =
        std::accumulate(sizePerThread.begin(), sizePerThread.end(), 1,
                        std::multiplies<unsigned>());
    SmallVector<unsigned> threadsPerDim(rank);
    for (unsigned k = 0; k < rank; ++k) {
      threadsPerDim[k] = ceil<unsigned>(shape[k], sizePerThread[k]);
    }
    for (unsigned n = 0; n < elemsPerThread; ++n) {
      unsigned linearNanoTileId = n / accumSizePerThread;
      unsigned linearElemsInNanoTileId = n % accumSizePerThread;
      SmallVector<unsigned> multiDimNanoTileId =
          getMultiDimIndex<unsigned>(linearNanoTileId, threadsPerDim);
      SmallVector<unsigned> multiElemsInNanoTileId =
          getMultiDimIndex<unsigned>(linearElemsInNanoTileId, sizePerThread);
      multiDimIdx[n].resize(rank);
      for (unsigned k = 0; k < rank; ++k) {
        unsigned reorderedMultiDimId =
            multiDimNanoTileId[k] *
                (sizePerThread[k] * threadsPerWarp[k] * warpsPerCTA[k]) +
            multiElemsInNanoTileId[k];
        multiDimIdx[n][k] = b.create<LLVM::AddOp>(
            loc, multiDimBase[k],
            createIndexAttrConstant(b, loc, llvmIndexTy,
                                    offset[k][reorderedMultiDimId]));
      }
    }

    return multiDimIdx;
  }

  Value getSharedMemoryBase(Location loc, ConversionPatternRewriter &rewriter,
                            Value smem, const Allocation *allocation,
                            Operation *op) const {
    auto ptrTy = LLVM::LLVMPointerType::get(
        this->getTypeConverter()->convertType(rewriter.getIntegerType(8)), 3);
    auto bufferId = allocation->getBufferId(op);
    assert(bufferId != Allocation::InvalidBufferId && "BufferId not found");
    size_t offset = allocation->getOffset(bufferId);
    auto llvmIndexTy = this->getTypeConverter()->getIndexType();
    Value offVal = createIndexAttrConstant(rewriter, loc, llvmIndexTy, offset);
    Value base = rewriter.create<LLVM::GEPOp>(loc, ptrTy, smem, offVal);
    return base;
  }
};

// Convert SplatOp or arith::ConstantOp with SplatElementsAttr to a
// LLVM::StructType value.
//
// @elemType: the element type in operand.
// @resType: the return type of the Splat-like op.
// @constVal: a LLVM::ConstantOp or other scalar value.
Value convertSplatLikeOp(Type elemType, Type resType, Value constVal,
                         TypeConverter *typeConverter,
                         ConversionPatternRewriter &rewriter, Location loc) {

  auto tensorTy = resType.cast<RankedTensorType>();
  auto layout = tensorTy.getEncoding();
  auto srcType = typeConverter->convertType(elemType);
  auto llSrc = rewriter.create<LLVM::BitcastOp>(loc, srcType, constVal);
  size_t numElemsPerThread = getElemsPerThread(layout, tensorTy.getShape());
  llvm::SmallVector<Value, 4> elems(numElemsPerThread, llSrc);
  llvm::SmallVector<Type, 4> elemTypes(elems.size(), srcType);
  auto structTy =
      LLVM::LLVMStructType::getLiteral(rewriter.getContext(), elemTypes);

  auto llStruct = getStructFromElements(loc, elems, rewriter, structTy);
  return llStruct;
}

struct SplatOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::SplatOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::SplatOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::SplatOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = op->getLoc();
    auto src = adaptor.src();
    auto llStruct = convertSplatLikeOp(src.getType(), op.getType(), src,
                                       getTypeConverter(), rewriter, loc);
    rewriter.replaceOp(op, {llStruct});
    return success();
  }
};

// This pattern helps to convert arith::ConstantOp(with SplatElementsAttr),
// the logic is the same as triton::SplatOp, so the underlying implementation
// is reused.
struct ArithConstantSplatOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<arith::ConstantOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      arith::ConstantOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(arith::ConstantOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto value = op.getValue();
    if (!value.dyn_cast<SplatElementsAttr>())
      return failure();

    auto loc = op->getLoc();

    LLVM::ConstantOp arithConstantOp;
    auto values = op.getValue().dyn_cast<SplatElementsAttr>();
    auto elemType = values.getElementType();

    Attribute val;
    if (type::isInt(elemType)) {
      val = values.getValues<IntegerAttr>()[0];
    } else if (type::isFloat(elemType)) {
      val = values.getValues<FloatAttr>()[0];
    } else {
      llvm::errs() << "ArithConstantSplatOpConversion get unsupported type: "
                   << value.getType() << "\n";
      return failure();
    }

    auto constOp = rewriter.create<LLVM::ConstantOp>(loc, elemType, val);
    auto llStruct = convertSplatLikeOp(elemType, op.getType(), constOp,
                                       getTypeConverter(), rewriter, loc);
    rewriter.replaceOp(op, llStruct);

    return success();
  }
};

// Contains some helper functions for both Load and Store conversions.
struct LoadStoreConversionBase : public ConvertTritonGPUOpToLLVMPatternBase {
  LoadStoreConversionBase(AxisInfoAnalysis &axisAnalysisPass)
      : AxisAnalysisPass(axisAnalysisPass) {}

  // Get corresponding LLVM element values of \param value.
  SmallVector<Value> getLLVMElems(Value value, Value llValue,
                                  const BlockedEncodingAttr &layout,
                                  TypeConverter *typeConverter,
                                  ConversionPatternRewriter &rewriter,
                                  Location loc) const {
    if (!value)
      return {};

    auto ty = value.getType().cast<RankedTensorType>();
    auto shape = ty.getShape();
    // Here, we assume that all inputs should have a blockedLayout

    unsigned valueElems = layout.getElemsPerThread(shape);

    auto llvmElemTy = typeConverter->convertType(ty.getElementType());
    auto llvmElemPtrPtrTy =
        LLVM::LLVMPointerType::get(LLVM::LLVMPointerType::get(llvmElemTy));

    auto valueVals = getElementsFromStruct(loc, llValue, valueElems, rewriter);
    return valueVals;
  }

  // Get the blocked layout.
  std::tuple<BlockedEncodingAttr, unsigned> getLayout(Value val) const {
    auto ty = val.getType().cast<RankedTensorType>();
    // Here, we assume that all inputs should have a blockedLayout
    auto layout = ty.getEncoding().dyn_cast<BlockedEncodingAttr>();
    assert(layout && "unexpected layout in getLayout");
    auto shape = ty.getShape();
    unsigned valueElems = layout.getElemsPerThread(shape);
    return std::make_tuple(layout, valueElems);
  }

  unsigned getAlignment(Value val, const BlockedEncodingAttr &layout) const {
    auto axisInfo = getAxisInfo(val);
    auto order = layout.getOrder();
    unsigned maxMultiple = axisInfo->getDivisibility(order[0]);
    unsigned maxContig = axisInfo->getContiguity(order[0]);
    unsigned alignment = std::min(maxMultiple, maxContig);
    return alignment;
  }

  unsigned getVectorizeSize(Value ptr,
                            const BlockedEncodingAttr &layout) const {
    auto axisInfo = getAxisInfo(ptr);
    // Here order should be ordered by contiguous first, so the first element
    // should have the largest contiguous.
    auto order = layout.getOrder();
    unsigned align = getAlignment(ptr, layout);

    auto ty = ptr.getType().dyn_cast<RankedTensorType>();
    assert(ty);
    auto shape = ty.getShape();

    unsigned contigPerThread = layout.getSizePerThread()[order[0]];
    unsigned vec = std::min(align, contigPerThread);
    vec = std::min<unsigned>(shape[order[0]], vec);

    return vec;
  }

  llvm::Optional<AxisInfo> getAxisInfo(Value val) const {
    if (auto it = AxisAnalysisPass.lookupLatticeElement(val)) {
      return it->getValue();
    }

    return llvm::Optional<AxisInfo>{};
  }

protected:
  AxisInfoAnalysis &AxisAnalysisPass;
};

struct StoreOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::StoreOp>,
      public LoadStoreConversionBase {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::StoreOp>::ConvertTritonGPUOpToLLVMPattern;

  StoreOpConversion(LLVMTypeConverter &converter,
                    AxisInfoAnalysis &axisAnalysisPass, PatternBenefit benefit)
      : ConvertTritonGPUOpToLLVMPattern<triton::StoreOp>(converter, benefit),
        LoadStoreConversionBase(axisAnalysisPass) {}

  LogicalResult
  matchAndRewrite(triton::StoreOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Value ptr = op.ptr();
    Value mask = op.mask();
    Value value = op.value();

    Value llPtr = adaptor.ptr();
    Value llMask = adaptor.mask();
    Value llValue = adaptor.value();

    auto loc = op->getLoc();
    MLIRContext *ctx = rewriter.getContext();

    auto valueTy = value.getType().dyn_cast<RankedTensorType>();
    if (!valueTy)
      return failure();
    Type valueElemTy =
        getTypeConverter()->convertType(valueTy.getElementType());

    auto [layout, numElems] = getLayout(ptr);

    auto ptrElems =
        getLLVMElems(ptr, llPtr, layout, getTypeConverter(), rewriter, loc);
    auto valueElems =
        getLLVMElems(value, llValue, layout, getTypeConverter(), rewriter, loc);
    assert(ptrElems.size() == valueElems.size());

    SmallVector<Value> maskElems;
    if (llMask) {
      maskElems =
          getLLVMElems(mask, llMask, layout, getTypeConverter(), rewriter, loc);
      assert(valueElems.size() == maskElems.size());
    }

    // Determine the vectorization size
    size_t vec = getVectorizeSize(ptr, layout);

    const size_t dtsize =
        std::max<int>(1, valueElemTy.getIntOrFloatBitWidth() / 8);
    const size_t valueElemNbits = dtsize * 8;

    const int numVecs = numElems / vec;
    for (size_t vecStart = 0; vecStart < numElems; vecStart += vec) {
      // TODO: optimization when ptr is AddPtr with constant offset
      size_t in_off = 0;

      const int maxWordWidth = std::max<int>(32, valueElemNbits);
      const int totalWidth = valueElemNbits * vec;
      const int width = std::min(totalWidth, maxWordWidth);
      const int nWords = std::max(1, totalWidth / width);
      const int wordNElems = width / valueElemNbits;
      const int vecNElems = totalWidth / valueElemNbits;
      assert(wordNElems * nWords * numVecs == numElems);

      // TODO(Superjomn) Add cache policy fields to StoreOp.
      // TODO(Superjomn) Deal with cache policy here.
      const bool hasL2EvictPolicy = false;

      PTXBuilder ptxBuilder;
      auto &ptxStoreInstr = *ptxBuilder.create<PtxIOInstr>("st");

      llvm::SmallVector<std::string> asmArgs;

      Type valArgTy = IntegerType::get(ctx, width);
      auto wordTy = VectorType::get(wordNElems, valueElemTy);

      auto *asmArgList = ptxBuilder.newListOperand();
      for (int wordIdx = 0; wordIdx < nWords; wordIdx++) {
        // llWord is a width-len composition
        Value llWord = rewriter.create<LLVM::UndefOp>(loc, wordTy);
        // Insert each value element to the composition
        for (int elemIdx = 0; elemIdx < wordNElems; elemIdx++) {
          const size_t elemOffset = vecStart + wordIdx * wordNElems + elemIdx;
          assert(elemOffset < valueElems.size());
          Value elem = valueElems[elemOffset];
          if (elem.getType().isInteger(1))
            elem = rewriter.create<LLVM::SExtOp>(loc, type::i8Ty(ctx), elem);
          elem = rewriter.create<LLVM::BitcastOp>(loc, valueElemTy, elem);

          llWord = rewriter.create<LLVM::InsertElementOp>(
              loc, wordTy, llWord, elem,
              rewriter.create<LLVM::ConstantOp>(
                  loc, type::u32Ty(ctx),
                  IntegerAttr::get(type::u32Ty(ctx), elemIdx)));
        }
        llWord = rewriter.create<LLVM::BitcastOp>(loc, valArgTy, llWord);
        std::string constraint =
            (width == 64) ? "l" : ((width == 32) ? "r" : "c");
        asmArgList->listAppend(ptxBuilder.newOperand(llWord, constraint));
      }

      // TODO(Superjomn) Need to check masks before vectorize the load for all
      // the values share one predicate? Here assume all the mask values are
      // the same.
      Value maskVal =
          llMask ? maskElems[vecStart]
                 : createLLVMIntegerConstant(rewriter, loc, getTypeConverter(),
                                             rewriter.getIntegerType(1), 1);
      ptxStoreInstr.global().b(width).v(nWords);

      auto *asmAddr =
          ptxBuilder.newAddrOperand(ptrElems[vecStart], "l", in_off);

      ptxStoreInstr(asmAddr, asmArgList).predicate(maskVal, "b");
      Type boolTy = getTypeConverter()->convertType(rewriter.getIntegerType(1));
      llvm::SmallVector<Type> argTys({boolTy, ptr.getType()});
      for (int i = 0; i < nWords; i++)
        argTys.push_back(valArgTy);

      auto ASMReturnTy = LLVM::LLVMVoidType::get(ctx);

      auto inlineAsm = rewriter.create<LLVM::InlineAsmOp>(
          loc, ASMReturnTy, ptxBuilder.getAllMLIRArgs(), // operands
          ptxBuilder.dump(),                             // asm_string
          ptxBuilder.getConstraints(),                   // constraints
          // TODO(Superjomn) determine the side effect.
          true,  // has_side_effects
          false, // is_align_stack
          LLVM::AsmDialectAttr::get(ctx,
                                    LLVM::AsmDialect::AD_ATT), // asm_dialect
          ArrayAttr::get(ctx, {})                              // operand_attrs
      );
    }
    rewriter.eraseOp(op);
    return success();
  }
};

struct BroadcastOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::BroadcastOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::BroadcastOp>::ConvertTritonGPUOpToLLVMPattern;

  // Following the order of indices in the legacy code, a broadcast of:
  //   [s(0), s(1) ... s(k-1),    1, s(k+1), s(k+2) ... s(n-1)]
  // =>
  //   [s(0), s(1) ... s(k-1), s(k), s(k+1), s(k+2) ... s(n-1)]
  //
  // logically maps to a broadcast within a thread's scope:
  //   [cta(0)..cta(k-1),     1,cta(k+1)..cta(n-1),spt(0)..spt(k-1),
  //   1,spt(k+1)..spt(n-1)]
  // =>
  //   [cta(0)..cta(k-1),cta(k),cta(k+1)..cta(n-1),spt(0)..spt(k-1),spt(k),spt(k+1)..spt(n-1)]
  //
  // regardless of the order of the layout
  //
  LogicalResult
  matchAndRewrite(triton::BroadcastOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    Value src = adaptor.src();
    Value result = op.result();
    auto srcTy = op.src().getType().cast<RankedTensorType>();
    auto resultTy = result.getType().cast<RankedTensorType>();
    auto srcLayout = srcTy.getEncoding().dyn_cast<BlockedEncodingAttr>();
    auto resultLayout = resultTy.getEncoding().dyn_cast<BlockedEncodingAttr>();
    assert(srcLayout && (srcLayout == resultLayout) &&
           "Unexpected layout of BroadcastOp");
    auto srcShape = srcTy.getShape();
    auto resultShape = resultTy.getShape();
    unsigned rank = srcTy.getRank();
    assert(rank == resultTy.getRank());

    SmallVector<int64_t, 4> srcLogicalShape(2 * rank);
    SmallVector<int64_t, 4> resultLogicalShape(2 * rank);
    SmallVector<unsigned, 2> broadcastDims;
    for (unsigned d = 0; d < rank; ++d) {
      unsigned resultShapePerCTA = resultLayout.getSizePerThread()[d] *
                                   resultLayout.getThreadsPerWarp()[d] *
                                   resultLayout.getWarpsPerCTA()[d];
      int64_t numCtas = ceil<unsigned>(resultShape[d], resultShapePerCTA);
      if (srcShape[d] != resultShape[d]) {
        assert(srcShape[d] == 1);
        broadcastDims.push_back(d);
        srcLogicalShape[d] = 1;
        srcLogicalShape[d + rank] =
            std::max(unsigned(1), srcLayout.getSizePerThread()[d]);
      } else {
        srcLogicalShape[d] = numCtas;
        srcLogicalShape[d + rank] = resultLayout.getSizePerThread()[d];
      }
      resultLogicalShape[d] = numCtas;
      resultLogicalShape[d + rank] = resultLayout.getSizePerThread()[d];
    }
    int64_t duplicates = 1;
    SmallVector<int64_t, 2> broadcastSizes(broadcastDims.size() * 2);
    for (auto it : llvm::enumerate(broadcastDims)) {
      // Incase there are multiple indices in the src that is actually
      // calculating the same element, srcLogicalShape may not need to be 1.
      // Such as the case when src of shape [256, 1], and with a blocked layout:
      // sizePerThread: [1, 4];  threadsPerWarp: [1, 32]; warpsPerCTA: [1, 2]
      int64_t d = resultLogicalShape[it.value()] / srcLogicalShape[it.value()];
      broadcastSizes[it.index()] = d;
      duplicates *= d;
      d = resultLogicalShape[it.value() + rank] /
          srcLogicalShape[it.value() + rank];
      broadcastSizes[it.index() + broadcastDims.size()] = d;
      duplicates *= d;
    }

    unsigned srcElems = srcLayout.getElemsPerThread(srcShape);
    auto elemTy = resultTy.getElementType();
    auto srcVals = getElementsFromStruct(loc, src, srcElems, rewriter);
    unsigned resultElems = resultLayout.getElemsPerThread(resultShape);
    SmallVector<Value> resultVals(resultElems);
    for (unsigned i = 0; i < srcElems; ++i) {
      auto srcMultiDim = getMultiDimIndex<int64_t>(i, srcLogicalShape);
      for (int64_t j = 0; j < duplicates; ++j) {
        auto resultMultiDim = srcMultiDim;
        auto bcastMultiDim = getMultiDimIndex<int64_t>(j, broadcastSizes);
        for (auto bcastDim : llvm::enumerate(broadcastDims)) {
          resultMultiDim[bcastDim.value()] += bcastMultiDim[bcastDim.index()];
          resultMultiDim[bcastDim.value() + rank] +=
              bcastMultiDim[bcastDim.index() + broadcastDims.size()] *
              srcLogicalShape[bcastDim.index() + broadcastDims.size()];
        }
        auto resultLinearIndex =
            getLinearIndex<int64_t>(resultMultiDim, resultLogicalShape);
        resultVals[resultLinearIndex] = srcVals[i];
      }
    }
    auto llvmStructTy = getTypeConverter()->convertType(resultTy);
    Value resultStruct =
        getStructFromElements(loc, resultVals, rewriter, llvmStructTy);
    rewriter.replaceOp(op, {resultStruct});
    return success();
  }
};

template <typename SourceOp>
struct ViewLikeOpConversion : public ConvertTritonGPUOpToLLVMPattern<SourceOp> {
  using OpAdaptor = typename SourceOp::Adaptor;
  explicit ViewLikeOpConversion(LLVMTypeConverter &typeConverter,
                                PatternBenefit benefit = 1)
      : ConvertTritonGPUOpToLLVMPattern<SourceOp>(typeConverter, benefit) {}

  LogicalResult
  matchAndRewrite(SourceOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // We cannot directly
    //   rewriter.replaceOp(op, adaptor.src());
    // due to MLIR's restrictions
    Location loc = op->getLoc();
    auto resultTy = op.getType().template cast<RankedTensorType>();
    auto resultShape = resultTy.getShape();
    unsigned elems = getElemsPerThread(resultTy.getEncoding(), resultShape);
    Type elemTy =
        this->getTypeConverter()->convertType(resultTy.getElementType());
    SmallVector<Type> types(elems, elemTy);
    Type structTy = LLVM::LLVMStructType::getLiteral(this->getContext(), types);
    auto vals =
        this->getElementsFromStruct(loc, adaptor.src(), elems, rewriter);
    Value view = getStructFromElements(loc, vals, rewriter, structTy);
    rewriter.replaceOp(op, view);
    return success();
  }
};

struct MakeRangeOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::MakeRangeOp> {

  MakeRangeOpConversion(LLVMTypeConverter &converter, PatternBenefit benefit)
      : ConvertTritonGPUOpToLLVMPattern<triton::MakeRangeOp>(converter,
                                                             benefit) {}

  LogicalResult
  matchAndRewrite(triton::MakeRangeOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    auto rankedTy = op.result().getType().dyn_cast<RankedTensorType>();
    auto shape = rankedTy.getShape();
    auto layout = rankedTy.getEncoding();

    auto elemTy = rankedTy.getElementType();
    assert(elemTy.isInteger(32));
    Value start = createIndexAttrConstant(rewriter, loc, elemTy, op.start());
    auto idxs = emitIndices(loc, rewriter, layout, shape);
    unsigned elems = idxs.size();
    SmallVector<Value> retVals(elems);
    for (auto multiDim : llvm::enumerate(idxs)) {
      assert(multiDim.value().size() == 1);
      retVals[multiDim.index()] =
          rewriter.create<LLVM::AddOp>(loc, multiDim.value()[0], start);
    }
    SmallVector<Type> types(elems, elemTy);
    Type structTy = LLVM::LLVMStructType::getLiteral(getContext(), types);
    Value result = getStructFromElements(loc, retVals, rewriter, structTy);
    rewriter.replaceOp(op, result);
    return success();
  }
};

struct LoadOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::LoadOp>,
      public LoadStoreConversionBase {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::LoadOp>::ConvertTritonGPUOpToLLVMPattern;

  LoadOpConversion(LLVMTypeConverter &converter,
                   AxisInfoAnalysis &axisAnalysisPass, PatternBenefit benefit)
      : ConvertTritonGPUOpToLLVMPattern<triton::LoadOp>(converter, benefit),
        LoadStoreConversionBase(axisAnalysisPass) {}

  LogicalResult
  matchAndRewrite(triton::LoadOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {

    Value ptr = op.ptr();
    Value mask = op.mask();
    Value other = op.other();

    Value llPtr = adaptor.ptr();
    Value llMask = adaptor.mask();
    Value llOther = adaptor.other();

    auto loc = op->getLoc();
    MLIRContext *ctx = rewriter.getContext();

    auto valueTy = op.getResult().getType().dyn_cast<RankedTensorType>();
    if (!valueTy)
      return failure();
    Type valueElemTy =
        getTypeConverter()->convertType(valueTy.getElementType());

    auto [layout, numElems] = getLayout(ptr);

    auto ptrElems =
        getLLVMElems(ptr, llPtr, layout, getTypeConverter(), rewriter, loc);
    assert(ptrElems.size() == numElems);

    SmallVector<Value> maskElems;
    if (llMask) {
      maskElems =
          getLLVMElems(mask, llMask, layout, getTypeConverter(), rewriter, loc);
      assert(ptrElems.size() == maskElems.size());
    }

    // Determine the vectorization size
    size_t vec = getVectorizeSize(ptr, layout);

    const size_t dtsize =
        std::max<int>(1, valueElemTy.getIntOrFloatBitWidth() / 8);
    const size_t valueElemNbits = dtsize * 8;

    const int numVecs = numElems / vec;

    // TODO: (goostavz) handle when other is const but not splat, which
    //       should be rarely seen
    bool otherIsSplatConstInt = false;
    DenseElementsAttr constAttr;
    int64_t splatVal = 0;
    if (valueElemTy.isa<IntegerType>() &&
        matchPattern(op.other(), m_Constant(&constAttr)) &&
        constAttr.isSplat()) {
      otherIsSplatConstInt = true;
      splatVal = constAttr.getSplatValue<APInt>().getSExtValue();
    }

    auto otherElems =
        getLLVMElems(other, llOther, layout, getTypeConverter(), rewriter, loc);

    SmallVector<Value> loadedVals;
    for (size_t vecStart = 0; vecStart < numElems; vecStart += vec) {
      // TODO: optimization when ptr is GEP with constant offset
      size_t in_off = 0;

      const int maxWordWidth = std::max<int>(32, valueElemNbits);
      const int totalWidth = valueElemNbits * vec;
      const int width = std::min(totalWidth, maxWordWidth);
      const int nWords = std::max(1, totalWidth / width);
      const int wordNElems = width / valueElemNbits;
      const int vecNElems = totalWidth / valueElemNbits;
      assert(wordNElems * nWords * numVecs == numElems);

      // TODO(Superjomn) Add cache policy fields to StoreOp.
      // TODO(Superjomn) Deal with cache policy here.
      const bool hasL2EvictPolicy = false;

      PTXBuilder ptxBuilder;
      auto &ld = *ptxBuilder.create<PtxIOInstr>("ld");

      // TODO(Superjomn) Need to check masks before vectorize the load for all
      // the values share one predicate? Here assume all the mask values are
      // the same.
      Value pred =
          mask ? maskElems[vecStart]
               : createLLVMIntegerConstant(rewriter, loc, getTypeConverter(),
                                           rewriter.getIntegerType(1), 1);

      const std::string readConstraint =
          (width == 64) ? "l" : ((width == 32) ? "r" : "c");
      const std::string writeConstraint =
          (width == 64) ? "=l" : ((width == 32) ? "=r" : "=c");

      // prepare asm operands
      auto *dstsOpr = ptxBuilder.newListOperand();
      for (int wordIdx = 0; wordIdx < nWords; wordIdx++) {
        auto *opr = ptxBuilder.newOperand(writeConstraint); // =r operations
        dstsOpr->listAppend(opr);
      }

      auto *addrOpr =
          ptxBuilder.newAddrOperand(ptrElems[vecStart], "l", in_off);

      // Define the instruction opcode
      ld.o("volatile", op.isVolatile())
          .global()
          .o("ca", op.cache() == triton::CacheModifier::CA)
          .o("cg", op.cache() == triton::CacheModifier::CG)
          .o("L1::evict_first",
             op.evict() == triton::EvictionPolicy::EVICT_FIRST)
          .o("L1::evict_last", op.evict() == triton::EvictionPolicy::EVICT_LAST)
          .o("L1::cache_hint", hasL2EvictPolicy)
          .v(nWords)
          .b(width);

      PTXBuilder::Operand *evictOpr{};

      // Here lack a mlir::Value to bind to this operation, so disabled.
      // if (has_l2_evict_policy)
      //   evictOpr = ptxBuilder.newOperand(l2Evict, "l");

      if (!evictOpr)
        ld(dstsOpr, addrOpr).predicate(pred, "b");
      else
        ld(dstsOpr, addrOpr, evictOpr).predicate(pred, "b");

      SmallVector<Value> others;
      if (other) {
        for (size_t ii = 0; ii < nWords; ii++) {
          PTXInstr &mov = *ptxBuilder.create<>("mov");
          mov.o("u", width);

          size_t size = width / valueElemNbits;

          auto vecTy = LLVM::getFixedVectorType(valueElemTy, size);
          Value v = rewriter.create<LLVM::UndefOp>(loc, vecTy);
          for (size_t s = 0; s < size; s++) {
            Value falseVal = otherElems[vecStart + ii * size + s];
            Value sVal = createIndexAttrConstant(
                rewriter, loc, this->getTypeConverter()->getIndexType(), s);
            v = rewriter.create<LLVM::InsertElementOp>(loc, vecTy, v, falseVal,
                                                       sVal);
          }
          v = rewriter.create<LLVM::BitcastOp>(
              loc, IntegerType::get(getContext(), width), v);

          PTXInstr::Operand *opr{};
          if (otherIsSplatConstInt) {
            opr = ptxBuilder.newConstantOperand(splatVal);
          } else {
            opr = ptxBuilder.newOperand(v, readConstraint);
            others.push_back(v);
          }

          mov(dstsOpr->listGet(ii), opr).predicateNot(pred, "b");
        }
      }

      // ---
      // create inline ASM signature
      // ---
      SmallVector<Type> retTys(nWords, IntegerType::get(getContext(), width));
      Type retTy = retTys.size() > 1
                       ? LLVM::LLVMStructType::getLiteral(getContext(), retTys)
                       : retTys[0];

      // TODO: if (has_l2_evict_policy)
      auto asmDialectAttr = LLVM::AsmDialectAttr::get(rewriter.getContext(),
                                                      LLVM::AsmDialect::AD_ATT);
      auto inlineAsmOp = rewriter.create<LLVM::InlineAsmOp>(
          loc, retTy, /*operands=*/ptxBuilder.getAllMLIRArgs(),
          /*asm_string=*/ptxBuilder.dump(),
          /*constraints=*/ptxBuilder.getConstraints(),
          /*has_side_effects=*/true,
          /*is_align_stack=*/false, /*asm_dialect=*/asmDialectAttr,
          /*operand_attrs=*/ArrayAttr());
      Value ret = inlineAsmOp.getResult(0);

      // ---
      // extract and store return values
      // ---
      SmallVector<Value> rets;
      for (unsigned int ii = 0; ii < nWords; ii++) {
        Value curr;
        if (retTy.isa<LLVM::LLVMStructType>()) {
          curr = rewriter.create<LLVM::ExtractValueOp>(
              loc, IntegerType::get(getContext(), width), ret,
              rewriter.getI64ArrayAttr(ii));
        } else {
          curr = ret;
        }
        curr = rewriter.create<LLVM::BitcastOp>(
            loc, LLVM::getFixedVectorType(valueElemTy, width / valueElemNbits),
            curr);
        rets.push_back(curr);
      }
      int tmp = (width / valueElemNbits);
      for (size_t ii = 0; ii < vec; ii++) {
        Value vecIdx = createIndexAttrConstant(
            rewriter, loc, this->getTypeConverter()->getIndexType(), ii % tmp);
        Value loaded = rewriter.create<LLVM::ExtractElementOp>(
            loc, valueElemTy, rets[ii / tmp], vecIdx);
        loadedVals.push_back(loaded);
      }
    } // end vec

    Type llvmResultStructTy = getTypeConverter()->convertType(valueTy);
    Value resultStruct =
        getStructFromElements(loc, loadedVals, rewriter, llvmResultStructTy);
    rewriter.replaceOp(op, {resultStruct});
    return success();
  }
};

struct GetProgramIdOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::GetProgramIdOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::GetProgramIdOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::GetProgramIdOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    Value blockId = rewriter.create<::mlir::gpu::BlockIdOp>(
        loc, rewriter.getIndexType(), ::mlir::gpu::Dimension::x);
    auto llvmIndexTy = getTypeConverter()->getIndexType();
    rewriter.replaceOpWithNewOp<UnrealizedConversionCastOp>(
        op, TypeRange{llvmIndexTy}, ValueRange{blockId});
    return success();
  }
};

struct AddPtrOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::AddPtrOp> {
  using ConvertTritonGPUOpToLLVMPattern<
      triton::AddPtrOp>::ConvertTritonGPUOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::AddPtrOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    auto resultTy = op.getType().dyn_cast<RankedTensorType>();
    auto resultLayout = resultTy.getEncoding().dyn_cast<BlockedEncodingAttr>();
    assert(resultLayout && "Unexpected resultLayout in AddPtrOpConversion");
    auto resultShape = resultTy.getShape();
    unsigned elems = resultLayout.getElemsPerThread(resultShape);
    Type elemTy =
        this->getTypeConverter()->convertType(resultTy.getElementType());
    SmallVector<Type> types(elems, elemTy);
    Type structTy = LLVM::LLVMStructType::getLiteral(getContext(), types);
    auto ptrs = getElementsFromStruct(loc, adaptor.ptr(), elems, rewriter);
    auto offsets =
        getElementsFromStruct(loc, adaptor.offset(), elems, rewriter);
    SmallVector<Value> resultVals(elems);
    for (unsigned i = 0; i < elems; ++i) {
      resultVals[i] =
          rewriter.create<LLVM::GEPOp>(loc, elemTy, ptrs[i], offsets[i]);
    }
    Value view = getStructFromElements(loc, resultVals, rewriter, structTy);
    rewriter.replaceOp(op, view);
    return success();
  }
};

template <typename SourceOp, typename DestOp>
class BinaryOpConversion : public ConvertTritonGPUOpToLLVMPattern<SourceOp> {
public:
  using OpAdaptor = typename SourceOp::Adaptor;

  explicit BinaryOpConversion(LLVMTypeConverter &typeConverter,
                              PatternBenefit benefit = 1)
      : ConvertTritonGPUOpToLLVMPattern<SourceOp>(typeConverter, benefit) {}

  LogicalResult
  matchAndRewrite(SourceOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto resultTy = op.getType().template dyn_cast<RankedTensorType>();
    // ArithmeticToLLVM will handle the lowering of scalar ArithOps
    if (!resultTy)
      return failure();

    Location loc = op->getLoc();
    auto resultLayout =
        resultTy.getEncoding().template dyn_cast<BlockedEncodingAttr>();
    auto resultShape = resultTy.getShape();
    assert(resultLayout && "Unexpected resultLayout in BinaryOpConversion");
    unsigned elems = resultLayout.getElemsPerThread(resultShape);
    Type elemTy =
        this->getTypeConverter()->convertType(resultTy.getElementType());
    SmallVector<Type> types(elems, elemTy);
    Type structTy = LLVM::LLVMStructType::getLiteral(this->getContext(), types);
    auto lhss =
        this->getElementsFromStruct(loc, adaptor.getLhs(), elems, rewriter);
    auto rhss =
        this->getElementsFromStruct(loc, adaptor.getRhs(), elems, rewriter);
    SmallVector<Value> resultVals(elems);
    for (unsigned i = 0; i < elems; ++i) {
      resultVals[i] = rewriter.create<DestOp>(loc, elemTy, lhss[i], rhss[i]);
    }
    Value view = getStructFromElements(loc, resultVals, rewriter, structTy);
    rewriter.replaceOp(op, view);
    return success();
  }
};

struct ConvertLayoutOpConversion
    : public ConvertTritonGPUOpToLLVMPattern<triton::gpu::ConvertLayoutOp> {
public:
  using ConvertTritonGPUOpToLLVMPattern<
      triton::gpu::ConvertLayoutOp>::ConvertTritonGPUOpToLLVMPattern;

  ConvertLayoutOpConversion(LLVMTypeConverter &converter,
                            const Allocation *allocation, Value smem,
                            PatternBenefit benefit)
      : ConvertTritonGPUOpToLLVMPattern<triton::gpu::ConvertLayoutOp>(converter,
                                                                      benefit),
        allocation_(allocation), smem_(smem) {}

  LogicalResult
  matchAndRewrite(triton::gpu::ConvertLayoutOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    Value src = op.src();
    Value dst = op.result();
    auto srcTy = src.getType().cast<RankedTensorType>();
    auto dstTy = dst.getType().cast<RankedTensorType>();
    Attribute srcLayout = srcTy.getEncoding();
    Attribute dstLayout = dstTy.getEncoding();
    if ((!srcLayout.isa<BlockedEncodingAttr>()) ||
        (!dstLayout.isa<BlockedEncodingAttr>())) {
      // TODO: not implemented
      assert(0 &&
             "convert_layout except for blocked -> blocked is not implemented");
      return failure();
    }
    auto llvmElemTy = getTypeConverter()->convertType(dstTy.getElementType());
    Value smemBase = getSharedMemoryBase(loc, rewriter, smem_, allocation_,
                                         op.getOperation());
    auto elemPtrTy = LLVM::LLVMPointerType::get(llvmElemTy, 3);
    smemBase = rewriter.create<LLVM::BitcastOp>(loc, elemPtrTy, smemBase);

    auto shape = dstTy.getShape();
    unsigned rank = dstTy.getRank();
    auto getContigPerThread = [&](const Attribute &layout,
                                  unsigned d) -> unsigned {
      if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>()) {
        return blockedLayout.getSizePerThread()[d];
      } else {
        assert(0 && "Unimplemented usage of getContigPerThread");
        return 0;
      }
    };
    auto getAccumElemsPerThread = [&](const Attribute &layout) -> unsigned {
      if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>()) {
        return product<unsigned>(blockedLayout.getSizePerThread());
      } else {
        assert(0 && "Unimplemented usage of getAccumElemsPerThread");
        return 0;
      }
    };
    auto getOrder = [&](const Attribute &layout) -> ArrayRef<unsigned> {
      if (auto blockedLayout = layout.dyn_cast<BlockedEncodingAttr>()) {
        return blockedLayout.getOrder();
      } else {
        assert(0 && "Unimplemented usage of getAccumElemsPerThread");
        return {};
      }
    };
    SmallVector<unsigned> numReplicates(rank);
    SmallVector<unsigned> inNumCTAsEachRep(rank);
    SmallVector<unsigned> outNumCTAsEachRep(rank);
    SmallVector<unsigned> inNumCTAs(rank);
    SmallVector<unsigned> outNumCTAs(rank);
    for (unsigned d = 0; d < rank; ++d) {
      unsigned inPerCTA =
          std::min(unsigned(shape[d]), getShapePerCTA(srcLayout, d));
      unsigned outPerCTA =
          std::min(unsigned(shape[d]), getShapePerCTA(dstLayout, d));
      unsigned maxPerCTA = std::max(inPerCTA, outPerCTA);
      numReplicates[d] = ceil<unsigned>(shape[d], maxPerCTA);
      inNumCTAsEachRep[d] = maxPerCTA / inPerCTA;
      outNumCTAsEachRep[d] = maxPerCTA / outPerCTA;
      // TODO: confirm this
      assert(maxPerCTA % inPerCTA == 0 && maxPerCTA % outPerCTA == 0);
      inNumCTAs[d] = ceil<unsigned>(shape[d], inPerCTA);
      outNumCTAs[d] = ceil<unsigned>(shape[d], outPerCTA);
    }
    // Potentially we need to store for multiple CTAs in this replication
    unsigned accumNumReplicates = product<unsigned>(numReplicates);
    unsigned accumInSizePerThread = getAccumElemsPerThread(srcLayout);
    unsigned elems = getElemsPerThread(srcLayout, srcTy.getShape());
    auto vals = getElementsFromStruct(loc, adaptor.src(), elems, rewriter);
    unsigned inVec = 0;
    unsigned outVec = 0;
    auto paddedRepShape = getScratchConfigForCvtLayout(op, inVec, outVec);

    unsigned outElems = getElemsPerThread(dstLayout, shape);
    auto outOrd = getOrder(dstLayout);
    SmallVector<Value> outVals(outElems);
    for (unsigned repId = 0; repId < accumNumReplicates; ++repId) {
      auto multiDimRepId = getMultiDimIndex<unsigned>(repId, numReplicates);
      rewriter.create<mlir::gpu::BarrierOp>(loc);
      if (auto srcBlockedLayout = srcLayout.dyn_cast<BlockedEncodingAttr>()) {
        processReplicaBlocked(loc, rewriter, /*stNotRd*/ true, srcTy,
                              inNumCTAsEachRep, multiDimRepId, inVec,
                              paddedRepShape, outOrd, vals, smemBase);
      } else {
        assert(0 && "ConvertLayout with input layout not implemented");
        return failure();
      }
      rewriter.create<mlir::gpu::BarrierOp>(loc);
      if (auto dstBlockedLayout = dstLayout.dyn_cast<BlockedEncodingAttr>()) {
        processReplicaBlocked(loc, rewriter, /*stNotRd*/ false, dstTy,
                              outNumCTAsEachRep, multiDimRepId, outVec,
                              paddedRepShape, outOrd, outVals, smemBase);
      } else {
        assert(0 && "ConvertLayout with output layout not implemented");
        return failure();
      }
    }

    SmallVector<Type> types(outElems, llvmElemTy);
    Type structTy = LLVM::LLVMStructType::getLiteral(getContext(), types);
    Value result = getStructFromElements(loc, outVals, rewriter, structTy);
    rewriter.replaceOp(op, result);
    return success();
  }

private:
  template <typename T>
  SmallVector<T> reorder(ArrayRef<T> input, ArrayRef<unsigned> order) const {
    size_t rank = order.size();
    assert(input.size() == rank);
    SmallVector<T> result(rank);
    for (auto it : llvm::enumerate(order)) {
      result[rank - 1 - it.value()] = input[it.index()];
    }
    return result;
  };

  void processReplicaBlocked(Location loc, ConversionPatternRewriter &rewriter,
                             bool stNotRd, RankedTensorType type,
                             ArrayRef<unsigned> numCTAsEachRep,
                             ArrayRef<unsigned> multiDimRepId, unsigned vec,
                             ArrayRef<unsigned> paddedRepShape,
                             ArrayRef<unsigned> outOrd,
                             SmallVector<Value> &vals, Value smemBase) const {
    unsigned accumNumCTAsEachRep = product<unsigned>(numCTAsEachRep);
    auto layout = type.getEncoding().cast<BlockedEncodingAttr>();
    auto rank = type.getRank();
    auto sizePerThread = layout.getSizePerThread();
    auto accumSizePerThread = product<unsigned>(sizePerThread);
    auto llvmIndexTy = getTypeConverter()->getIndexType();
    SmallVector<unsigned> numCTAs(rank);
    SmallVector<unsigned> shapePerCTA(rank);
    for (unsigned d = 0; d < rank; ++d) {
      shapePerCTA[d] = layout.getSizePerThread()[d] *
                       layout.getThreadsPerWarp()[d] *
                       layout.getWarpsPerCTA()[d];
      numCTAs[d] = ceil<unsigned>(type.getShape()[d], shapePerCTA[d]);
    }
    auto llvmElemTy = getTypeConverter()->convertType(type.getElementType());
    auto multiDimOffsetFirstElem =
        emitBaseIndexForBlockedLayout(loc, rewriter, layout, type.getShape());
    for (unsigned ctaId = 0; ctaId < accumNumCTAsEachRep; ++ctaId) {
      auto multiDimCTAInRepId =
          getMultiDimIndex<unsigned>(ctaId, numCTAsEachRep);
      SmallVector<unsigned> multiDimCTAId(rank);
      for (auto it : llvm::enumerate(multiDimCTAInRepId)) {
        auto d = it.index();
        multiDimCTAId[d] = multiDimRepId[d] * numCTAsEachRep[d] + it.value();
      }

      unsigned linearCTAId = getLinearIndex<unsigned>(multiDimCTAId, numCTAs);
      // TODO: This is actually redundant index calculation, we should
      //       consider of caching the index calculation result in case
      //       of performance issue observed.
      // for (unsigned elemId = linearCTAId * accumSizePerThread;
      //      elemId < (linearCTAId + 1) * accumSizePerThread; elemId += vec) {
      for (unsigned elemId = 0; elemId < accumSizePerThread; elemId += vec) {
        auto multiDimElemId =
            getMultiDimIndex<unsigned>(elemId, layout.getSizePerThread());
        SmallVector<Value> multiDimOffset(rank);
        for (unsigned d = 0; d < rank; ++d) {
          multiDimOffset[d] = rewriter.create<LLVM::AddOp>(
              loc, multiDimOffsetFirstElem[d],
              createIndexAttrConstant(rewriter, loc, llvmIndexTy,
                                      multiDimCTAInRepId[d] * shapePerCTA[d] +
                                          multiDimElemId[d]));
        }
        Value offset =
            linearize(rewriter, loc, reorder<Value>(multiDimOffset, outOrd),
                      reorder<unsigned>(paddedRepShape, outOrd));
        auto elemPtrTy = LLVM::LLVMPointerType::get(llvmElemTy, 3);
        Value ptr =
            rewriter.create<LLVM::GEPOp>(loc, elemPtrTy, smemBase, offset);
        auto vecTy = VectorType::get(vec, llvmElemTy);
        ptr = rewriter.create<LLVM::BitcastOp>(
            loc, LLVM::LLVMPointerType::get(vecTy, 3), ptr);
        if (stNotRd) {
          Value valVec = rewriter.create<LLVM::UndefOp>(loc, vecTy);
          for (unsigned v = 0; v < vec; ++v) {
            Value vVal = createIndexAttrConstant(
                rewriter, loc, getTypeConverter()->getIndexType(), v);
            valVec = rewriter.create<LLVM::InsertElementOp>(
                loc, vecTy, valVec,
                vals[elemId + linearCTAId * accumSizePerThread + v], vVal);
          }
          rewriter.create<LLVM::StoreOp>(loc, valVec, ptr);
        } else {
          Value valVec = rewriter.create<LLVM::LoadOp>(loc, ptr);
          for (unsigned v = 0; v < vec; ++v) {
            Value vVal = createIndexAttrConstant(
                rewriter, loc, getTypeConverter()->getIndexType(), v);
            vals[elemId + linearCTAId * accumSizePerThread + v] =
                rewriter.create<LLVM::ExtractElementOp>(loc, llvmElemTy, valVec,
                                                        vVal);
          }
        }
      }
    }
  }

  const Allocation *allocation_;
  Value smem_;
};

class TritonGPUToLLVMTypeConverter : public LLVMTypeConverter {
public:
  using TypeConverter::convertType;

  TritonGPUToLLVMTypeConverter(MLIRContext *ctx, LowerToLLVMOptions &option,
                               const DataLayoutAnalysis *analysis = nullptr)
      : LLVMTypeConverter(ctx, option, analysis) {
    addConversion([&](triton::PointerType type) -> llvm::Optional<Type> {
      return convertTritonPointerType(type);
    });
    addConversion([&](RankedTensorType type) -> llvm::Optional<Type> {
      return convertTritonTensorType(type);
    });
  }

  Type convertTritonPointerType(triton::PointerType type) {
    return LLVM::LLVMPointerType::get(type.getPointeeType(),
                                      type.getAddressSpace());
  }

  llvm::Optional<Type> convertTritonTensorType(RankedTensorType type) {
    Attribute layout = type.getEncoding();
    if (layout && (layout.isa<BlockedEncodingAttr>() ||
                   layout.isa<SliceEncodingAttr>())) {
      unsigned numElementsPerThread =
          getElemsPerThread(layout, type.getShape());
      SmallVector<Type, 4> types(numElementsPerThread,
                                 convertType(type.getElementType()));
      return LLVM::LLVMStructType::getLiteral(&getContext(), types);
    } else if (auto mma_layout = layout.dyn_cast<MmaEncodingAttr>()) {
      // TODO: Not implemented
      return llvm::None;
    } else if (auto shared_layout = layout.dyn_cast<SharedEncodingAttr>()) {
      // TODO: Not implemented
      return llvm::None;
    }
    return llvm::None;
  }
};

void populateTritonToLLVMPatterns(mlir::LLVMTypeConverter &typeConverter,
                                  RewritePatternSet &patterns, int numWarps,
                                  AxisInfoAnalysis &axisInfoAnalysis,
                                  const Allocation *allocation, Value smem,
                                  PatternBenefit benefit = 1) {
  patterns.add<ArithConstantSplatOpConversion>(typeConverter, benefit);
  patterns.add<BinaryOpConversion<arith::AddIOp, LLVM::AddOp>>(typeConverter,
                                                               benefit);
  patterns.add<BinaryOpConversion<arith::AddFOp, LLVM::FAddOp>>(typeConverter,
                                                                benefit);
  patterns.add<BinaryOpConversion<arith::MulIOp, LLVM::MulOp>>(typeConverter,
                                                               benefit);
  patterns.add<BinaryOpConversion<arith::MulFOp, LLVM::FMulOp>>(typeConverter,
                                                                benefit);
  patterns.add<BroadcastOpConversion>(typeConverter, benefit);
  patterns.add<AddPtrOpConversion>(typeConverter, benefit);
  patterns.add<ConvertLayoutOpConversion>(typeConverter, allocation, smem,
                                          benefit);
  patterns.add<GetProgramIdOpConversion>(typeConverter, benefit);
  patterns.add<LoadOpConversion>(typeConverter, axisInfoAnalysis, benefit);
  patterns.add<MakeRangeOpConversion>(typeConverter, benefit);
  patterns.add<ReturnOpConversion>(typeConverter, benefit);
  patterns.add<SplatOpConversion>(typeConverter, benefit);
  patterns.add<StoreOpConversion>(typeConverter, axisInfoAnalysis, benefit);
  patterns.add<ViewLikeOpConversion<triton::ViewOp>>(typeConverter, benefit);
  patterns.add<ViewLikeOpConversion<triton::ExpandDimsOp>>(typeConverter,
                                                           benefit);
}

class ConvertTritonGPUToLLVM
    : public ConvertTritonGPUToLLVMBase<ConvertTritonGPUToLLVM> {
public:
  ConvertTritonGPUToLLVM() = default;

  void runOnOperation() override {
    MLIRContext *context = &getContext();
    ModuleOp mod = getOperation();

    mlir::LowerToLLVMOptions option(context);
    // TODO: need confirm
    option.overrideIndexBitwidth(32);
    TritonGPUToLLVMTypeConverter typeConverter(context, option);
    TritonLLVMFunctionConversionTarget funcTarget(*context, typeConverter);
    TritonLLVMConversionTarget target(*context, typeConverter);

    int numWarps = triton::gpu::TritonGPUDialect::getNumWarps(mod);

    // step 1: Convert FuncOp to LLVMFuncOp via partial conversion
    // step 2: Allocate for shared memories
    // step 3: Convert the rest of ops via partial conversion
    // The reason for a seperation between 1/3 is that, step 2 is out of
    // the scope of Dialect Conversion, thus we need to make sure the smem_
    // is not revised during the conversion of step 3.
    RewritePatternSet func_patterns(context);
    func_patterns.add<FuncOpConversion>(typeConverter, numWarps, 1 /*benefit*/);
    if (failed(
            applyPartialConversion(mod, funcTarget, std::move(func_patterns))))
      return signalPassFailure();

    Allocation allocation(mod);
    auto axisAnalysis = runAxisAnalysis(mod);
    initSharedMemory(allocation.getSharedMemorySize(), typeConverter);

    // We set a higher benefit here to ensure triton's patterns runs before
    // arith patterns for some encoding not supported by the community
    // patterns.
    RewritePatternSet patterns(context);
    populateTritonToLLVMPatterns(typeConverter, patterns, numWarps,
                                 *axisAnalysis, &allocation, smem_,
                                 10 /*benefit*/);

    // Add arith/math's patterns to help convert scalar expression to LLVM.
    mlir::arith::populateArithmeticToLLVMConversionPatterns(typeConverter,
                                                            patterns);
    mlir::populateMathToLLVMConversionPatterns(typeConverter, patterns);

    mlir::populateGpuToNVVMConversionPatterns(typeConverter, patterns);

    if (failed(applyPartialConversion(mod, target, std::move(patterns))))
      return signalPassFailure();
  }

protected:
  std::unique_ptr<AxisInfoAnalysis> runAxisAnalysis(ModuleOp module) {
    auto axisAnalysisPass =
        std::make_unique<AxisInfoAnalysis>(module->getContext());
    axisAnalysisPass->run(module);

    return axisAnalysisPass;
  }

  void initSharedMemory(size_t size,
                        TritonGPUToLLVMTypeConverter &typeConverter);

  Value smem_;
};

void ConvertTritonGPUToLLVM::initSharedMemory(
    size_t size, TritonGPUToLLVMTypeConverter &typeConverter) {
  ModuleOp mod = getOperation();
  OpBuilder b(mod.getBodyRegion());
  auto loc = mod.getLoc();
  auto elemTy = typeConverter.convertType(b.getIntegerType(8));
  auto arrayTy = LLVM::LLVMArrayType::get(elemTy, size);
  auto global = b.create<LLVM::GlobalOp>(
      loc, arrayTy, /*isConstant=*/false, LLVM::Linkage::Internal,
      "global_smem", /*value=*/Attribute(),
      /*alignment=*/0, mlir::gpu::GPUDialect::getWorkgroupAddressSpace());
  SmallVector<LLVM::LLVMFuncOp> funcs;
  mod.walk([&](LLVM::LLVMFuncOp func) { funcs.push_back(func); });
  assert(funcs.size() == 1 &&
         "Inliner pass is expected before TritonGPUToLLVM");
  b.setInsertionPointToStart(&funcs[0].getBody().front());
  smem_ = b.create<LLVM::AddressOfOp>(loc, global);
}

} // namespace

namespace mlir {

TritonLLVMConversionTarget::TritonLLVMConversionTarget(
    MLIRContext &ctx, mlir::LLVMTypeConverter &typeConverter)
    : ConversionTarget(ctx), typeConverter(typeConverter) {
  addLegalDialect<LLVM::LLVMDialect>();
  addLegalDialect<NVVM::NVVMDialect>();
  // addIllegalDialect<triton::TritonDialect>();
  // addIllegalDialect<triton::gpu::TritonGPUDialect>();
  addIllegalDialect<mlir::gpu::GPUDialect>();
  addLegalOp<mlir::UnrealizedConversionCastOp>();
}

TritonLLVMFunctionConversionTarget::TritonLLVMFunctionConversionTarget(
    MLIRContext &ctx, mlir::LLVMTypeConverter &typeConverter)
    : ConversionTarget(ctx), typeConverter(typeConverter) {
  addLegalDialect<LLVM::LLVMDialect>();
  // addLegalDialect<NVVM::NVVMDialect>();
  addIllegalOp<mlir::FuncOp>();
  addLegalOp<mlir::UnrealizedConversionCastOp>();
}

namespace triton {

std::unique_ptr<OperationPass<ModuleOp>> createConvertTritonGPUToLLVMPass() {
  return std::make_unique<::ConvertTritonGPUToLLVM>();
}

} // namespace triton
} // namespace mlir