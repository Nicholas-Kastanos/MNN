//
//  DepthwiseConvWeightMerge.cpp
//  MNNConverter
//
//  Created by MNN on 2021/04/19.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#include "../TemplateMerge.hpp"
#include "MNN/expr/MathOp.hpp"
#include "MNN/expr/NeuralNetWorkOp.hpp"
#include "MNN_generated.h"
#include "../../common/Global.hpp"
#include "config.hpp"
namespace MNN {
namespace Express {

static auto gRegister = []() {

    auto match = [](EXPRP expr) {
        if (nullptr == expr->get()) {
            return false;
        }
        if (expr->get()->type() != OpType_ConvolutionDepthwise) {
            return false;
        }
        // 1. input, 2. weight, 3. bias
        auto inputs = expr->inputs();
        if (inputs.size() < 2) {
            return false;
        }
        if (inputs.size() >= 2) {
            auto weightVar  = inputs[1];
            auto weightInfo = weightVar->getInfo();
            auto weightPtr  = weightVar->readMap<float>();
            if (nullptr == weightInfo || nullptr == weightPtr) {
                return false;
            }
        }
        if (inputs.size() == 3) {
            auto biasVar  = inputs[1];
            auto biasInfo = biasVar->getInfo();
            auto biasPtr  = biasVar->readMap<float>();
            if (nullptr == biasInfo || nullptr == biasPtr) {
                return false;
            }
        }
        return true;
    };

    auto transform = [](EXPRP expr) {
        std::unique_ptr<OpT> convOp(expr->get()->UnPack());
        auto inputs = expr->inputs();
        if (inputs.size() >= 2) {
            auto weightVar   = inputs[1];
            auto weightInfo  = weightVar->getInfo();
            auto weightPtr   = weightVar->readMap<float>();
            auto& weightData = convOp->main.AsConvolution2D()->weight;
            weightData.resize(weightInfo->size);
            memcpy(weightData.data(), weightPtr, weightInfo->size * sizeof(float));
        }
        if (inputs.size() == 3) {
            auto biasVar   = inputs[2];
            auto biasInfo  = biasVar->getInfo();
            auto biasPtr   = biasVar->readMap<float>();
            auto& biasData = convOp->main.AsConvolution2D()->weight;
            biasData.resize(biasInfo->size);
            memcpy(biasData.data(), biasPtr, biasInfo->size * sizeof(float));
        }
        auto newExpr = Expr::create(convOp.get(), {inputs[0]});
        newExpr->setName(expr->name());
        Expr::replace(expr, newExpr);
        return true;
    };

    TemplateMerge::getInstance("Merge").insertTemplate("DepthwiseConvWeightMerge", match, transform);
    return true;
}();

}
} // namespace MNN
