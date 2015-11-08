//
// <copyright file="LinearAlgebraNodes.h" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//
#pragma once

#include <unordered_set>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <list>
#include <memory>
#include <algorithm>
#include <assert.h>
#include <atomic>
#include <sstream>
#include <iostream>

#include "Basics.h"
#include "Matrix.h"
#include "ComputationNode.h"

namespace Microsoft { namespace MSR { namespace CNTK {

    // -----------------------------------------------------------------------
    // PlusNode (summand1, summand2)
    // -----------------------------------------------------------------------

    template<class ElemType>
    class PlusNode : public ComputationNode<ElemType>, public NumInputs<2>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"Plus"; }
    public:
        PlusNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange) override
        {
            Matrix<ElemType> gradientValues = GradientSlice(frameRange);
            Matrix<ElemType> functionValues = ValueSlice(frameRange);
            Matrix<ElemType> inputGradientValues = Inputs(inputIndex)->GradientSlice(frameRange);
            Matrix<ElemType> inputFunctionValues = Inputs(inputIndex)->ValueSlice(frameRange);

#if DUMPOUTPUT
            functionValues.Print("PlusNode");
#endif
            size_t rowsc = inputFunctionValues.GetNumRows(), colsc = inputFunctionValues.GetNumCols();
            size_t rowsp = functionValues.GetNumRows(),      colsp = functionValues.GetNumCols();
#if DUMPOUTPUT
            fprintf(stderr, "input dimensions %lld x %lld,  this node dimensions %lld x %lld\n", rowsc, colsc, rowsp, colsp);
            gradientValues.Print("Gradient-in");
            inputGradientValues.Print("child Gradient-in/out");
#endif

            if (colsc == colsp && rowsc == rowsp)                   // matching dimensions  --this may also trigger for column vector added to a frame, if frameRange denotes a single frame
            {
                // BUGBUG: if we reduce from a frame of a MB into a one-column vector, then we must also mask gaps
                inputGradientValues += gradientValues;
            }
            else if (colsc == 1 && rowsc == 1)                      // child is a scalar
            {
                MaskMissingGradientColumnsToZero(frameRange);       // reducing over frames, so we must zero out the gaps
                inputGradientValues += gradientValues.SumOfElements();
            }
            else if (colsc == 1 && colsp != 1)                      // child is a broadcasting column vector
            {
                size_t colspExpand = rowsp*colsp/rowsc;
                MaskMissingGradientColumnsToZero(frameRange);       // reducing over frames, so we must zero out the gaps
                Matrix<ElemType>::MultiplyAndAdd(gradientValues.Reshaped(rowsc, colspExpand), false, ConstOnes(colspExpand, 1, functionValues.GetDeviceId()), false, inputGradientValues);
            }
            else if (rowsc == 1 && rowsp != 1)                      // child is a broadcasting row vector
            {
                Matrix<ElemType>::MultiplyAndAdd(ConstOnes(1, rowsp, functionValues.GetDeviceId()), false, gradientValues, false, inputGradientValues);
            }
            else if (colsc != 1 && colsp % colsc == 0)
            {
                // the children matrix is [a b] and the parent considers it as [a a a b b b]
                // Note: There is no need to mask gaps here because this operation is only allowed on non-MBLayout inputs
                size_t ratio = colsp / colsc; 
                for (size_t i = 0; i < colsc; i++)
                {
                    size_t colspExpand = rowsp*colsp / rowsc / colsc;
                    Matrix<ElemType> tmp = gradientValues.ColumnSlice(i * ratio, ratio);
                    tmp.Reshape(rowsc, colspExpand);
                    Matrix<ElemType> res = inputGradientValues.ColumnSlice(i, 1);
                    Matrix<ElemType>::MultiplyAndAdd(tmp, false, ConstOnes(colspExpand, 1, functionValues.GetDeviceId()), false, res);
                    inputGradientValues.ColumnSlice(i, 1).SetValue(res);
                }
            }
            else
                RuntimeError("Plus partial: unexpected condition.");
#if DUMPOUTPUT
            inputGradientValues.Print("child Gradient-out");
#endif
        }

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override  
        {
            Matrix<ElemType> functionValues = ValueSliceToDense(frameRange, false); // Switch to dense as a work-around because ColumnSlice doesn't support all the sparse formats
            Matrix<ElemType> inputFunctionValues0 = Inputs(0)->ValueSlice(frameRange);
            Matrix<ElemType> inputFunctionValues1 = Inputs(1)->ValueSlice(frameRange);
            // Note: If one input is a column vector (no MBLayout) and the other a sequence of frames (MBLayout), then the above will be a slice for the other only.

            size_t rows0 = inputFunctionValues0.GetNumRows(), cols0 = inputFunctionValues0.GetNumCols();
            size_t rows1 = inputFunctionValues1.GetNumRows(), cols1 = inputFunctionValues1.GetNumCols();

            if ((rows0 == rows1 && cols0 == cols1/*matching dimensions*/) || ((rows0 == 1 || rows1 == 1)/*one is a broadcasting row vector*/ && cols0 == cols1))
            {
                functionValues.AssignSumOf(inputFunctionValues0, inputFunctionValues1);
            }
            else if (cols0 == 1 && rows1 % rows0 == 0)  // one is col vec with divisable rows, including scalar   --allowing divisable rows can be useful for images
            {
                functionValues.AssignSumOf(inputFunctionValues0, inputFunctionValues1.Reshaped(rows0, rows1 * cols1 / rows0));
                functionValues.Reshape(max(rows0, rows1), max(cols0, cols1));
            }
            else if (cols1 == 1 && rows0 % rows1 == 0)  // one is col vec with divisable rows, including scalar
            {
                functionValues.AssignSumOf(inputFunctionValues0.Reshaped(rows1, rows0 * cols0 / rows1), inputFunctionValues1);
                functionValues.Reshape(max(rows0, rows1), max(cols0, cols1));
            }       
            else if (cols1 < cols0 && rows0 == rows1 && cols0 % cols1 == 0)  // first summand is a matrix with number of columns that is a multiple of the column number of the second matrix
            {
                if (m_pMBLayout)
                    InvalidArgument("%ls %ls operation applied to mismatching number of columns when columns are samples of a minibatch");
                // the children matrix is [a b] and the parent considers it as [a a a b b b]
                // This can be useful for dealing with images.
                Matrix<ElemType> tmpMat(inputFunctionValues1.GetDeviceId());
                size_t ratio = cols0 / cols1;
                // TODO: Why is this different from MinusNode?
                for (size_t i = 0; i < cols1; i++)
                {
                    tmpMat = Matrix<ElemType>::RepMat(inputFunctionValues1.ColumnSlice(i, 1), 1, ratio);
                    functionValues.ColumnSlice(i*ratio, ratio).SetValue(tmpMat + inputFunctionValues0.ColumnSlice(i * ratio, ratio)); 
                }
            }
            else
                LogicError("%ls %ls operation's Validate() function let invalid dimensions slip by.", NodeName().c_str(), OperationName().c_str());
#if DUMPOUTPUT
            functionValues.Print("PlusNode");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            ValidateBinaryZip(isFinalValidationPass, true/*allowMultiples*/);
        }

        virtual void InferImageDimsFromInputs() //based on the matrix with larger size
        {
            size_t rows0 = Inputs(0)->GetNumRows(), cols0 = Inputs(0)->GetNumCols();
            size_t rows1 = Inputs(1)->GetNumRows(), cols1 = Inputs(1)->GetNumCols();

            if (rows0 > rows1 || cols0 > cols1) //child 0 is larger
                InferImageDimsFromInput(0);
            else if (rows0 < rows1 || cols0 < cols1) //child 1 is larger
                InferImageDimsFromInput(1);
            else //same size
            {
                if (IsChildAnImage(0))  //when conflict, give priority to child 0
                    InferImageDimsFromInput(0);
                else
                    InferImageDimsFromInput(1);
            }
        }
    };

    template class PlusNode<float>; 
    template class PlusNode<double>;

    // -----------------------------------------------------------------------
    // MinusNode (minuend, subtrahend)
    // -----------------------------------------------------------------------

    // TODO: merge with PlusNode
    template<class ElemType>
    class MinusNode : public ComputationNode<ElemType>, public NumInputs<2>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"Minus"; }
    public:
        MinusNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange) override
        {
            Matrix<ElemType> gradientValues = GradientSlice(frameRange);
            Matrix<ElemType> functionValues = ValueSlice(frameRange);

            Matrix<ElemType> childGradientValues = Inputs(inputIndex)->GradientSlice(frameRange);
            Matrix<ElemType> childFunctionValues = Inputs(inputIndex)->ValueSlice(frameRange);

            size_t rowsc = childFunctionValues.GetNumRows(), colsc = childFunctionValues.GetNumCols();
            size_t rowsp = functionValues.GetNumRows(),      colsp = functionValues.GetNumCols();

            ElemType sign = inputIndex == 0 ? 1.0f : -1.0f;
            if (colsc == colsp && rowsc == rowsp)                   // matching dimensions
            {
                // BUGBUG: if we reduce from a frame of a MB into a one-column vector, then we must also mask gaps
                if (sign > 0)
                    childGradientValues += gradientValues;
                else
                    childGradientValues -= gradientValues;
            }
            else if (colsc == 1 && rowsc == 1)                      // child is a scalar (1 x 1)
            {
                MaskMissingGradientColumnsToZero(frameRange);       // reducing over frames, so we must zero out the gaps
                if (sign > 0)
                    childGradientValues += gradientValues.SumOfElements();
                else
                    childGradientValues -= gradientValues.SumOfElements();
            }
            else if (colsc == 1 && colsp != 1)                      // child is broadcasting column vector
            {
                size_t colspExpand = rowsp * colsp / rowsc;
                MaskMissingGradientColumnsToZero(frameRange);       // reducing over frames, so we must zero out the gaps
                Matrix<ElemType>::MultiplyAndWeightedAdd(sign, gradientValues.Reshaped(rowsc, colspExpand), false, ConstOnes(colspExpand, 1, FunctionValues().GetDeviceId()), false, 1, childGradientValues);
            }
            else if (rowsc == 1 && rowsp != 1)                      // child is a broadcasting row vector
            {
                Matrix<ElemType>::MultiplyAndWeightedAdd(sign, ConstOnes(1, rowsp, FunctionValues().GetDeviceId()), false, gradientValues, false, 1, childGradientValues);
            }
            else
                LogicError("%ls %ls operation's Validate() function let invalid dimensions slip by.", NodeName().c_str(), OperationName().c_str());
        }

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override
        {
            Matrix<ElemType> functionValues = ValueSlice(frameRange);
            Matrix<ElemType> inputFunctionValues0 = Inputs(0)->ValueSlice(frameRange);
            Matrix<ElemType> inputFunctionValues1 = Inputs(1)->ValueSlice(frameRange);

            size_t rows0 = inputFunctionValues0.GetNumRows(), cols0 = inputFunctionValues0.GetNumCols();
            size_t rows1 = inputFunctionValues1.GetNumRows(), cols1 = inputFunctionValues1.GetNumCols();
            functionValues.VerifySize(max(rows0, rows1), max(cols0,cols1));

            if ((rows0 == rows1 && cols0 == cols1/*match*/) || ((rows0 == 1 || rows1 == 1)/*one is a broadcasting row vector*/ && cols0 == cols1))
            {
                functionValues.AssignDifferenceOf(inputFunctionValues0, inputFunctionValues1);
            }
            else if (cols0 == 1 && rows1 % rows0 == 0)  // one is col vec with divisable rows, including scalar
            {
                functionValues.AssignDifferenceOf(inputFunctionValues0, inputFunctionValues1.Reshaped(rows0, rows1 * cols1 / rows0));
                functionValues.Reshape(max(rows0, rows1), max(cols0,cols1));
            }
            else if (cols1 == 1 && rows0 % rows1 == 0)  // one is col vec with divisable rows, including scalar
            {
                functionValues.AssignDifferenceOf(inputFunctionValues0.Reshaped(rows1, rows0 * cols0 / rows1), inputFunctionValues1);
                functionValues.Reshape(max(rows0, rows1), max(cols0, cols1));
            }
            else
                LogicError("%ls %ls operation's Validate() function let invalid dimensions slip by.", NodeName().c_str(), OperationName().c_str());
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            ValidateBinaryZip(isFinalValidationPass, true/*allowMultiples*/);
        }

        virtual void InferImageDimsFromInputs() //based on the matrix with larger size
        {
            size_t rows0 = Inputs(0)->GetNumRows(), cols0 = Inputs(0)->GetNumCols();
            size_t rows1 = Inputs(1)->GetNumRows(), cols1 = Inputs(1)->GetNumCols();

            if (rows0 > rows1 || cols0 > cols1) //child 0 is larger
                InferImageDimsFromInput(0);
            else if (rows0 < rows1 || cols0 < cols1) //child 1 is larger
                InferImageDimsFromInput(1);
            else //same size
            {
                if (IsChildAnImage(0))  //when conflict, give priority to child 0
                    InferImageDimsFromInput(0);
                else
                    InferImageDimsFromInput(1);
            }
        }
    };

    template class MinusNode<float>; 
    template class MinusNode<double>;

    // -----------------------------------------------------------------------
    // ScaleNode (scalar scaling factor, matrix)
    // -----------------------------------------------------------------------

    template<class ElemType>
    class ScaleNode : public ComputationNode<ElemType>, public NumInputs<2>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"Scale"; }
    public:
        ScaleNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange) override
        {
            if (inputIndex == 0)        // left derivative
            {
                // this is a reduction over frames, so we must mask gaps to zero
                Inputs(0)->GradientValues() += Matrix<ElemType>::InnerProductOfMatrices(MaskedGradientSlice(frameRange), Inputs(1)->MaskedValueSlice(frameRange)); // element-wise product summed up over all
            }
            else if (inputIndex == 1)   // right derivative
            {
                Matrix<ElemType> sliceInput1Grad = Inputs(1)->GradientSlice(frameRange);
                Matrix<ElemType>::ScaleAndAdd(Inputs(0)->FunctionValues().Get00Element(), GradientSlice(frameRange), sliceInput1Grad);
            }
        }

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override  
        {
            ValueSlice(frameRange).AssignProductOf(Inputs(0)->FunctionValues().Get00Element(), Inputs(1)->ValueSlice(frameRange));
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);

            // left Node must be a scalar
            if (isFinalValidationPass && (Inputs(0)->GetNumRows() != 1 || Inputs(0)->GetNumCols() != 1))
                RuntimeError("The left value of ScaleNode must be a scalar value.");

            Resize(Inputs(1));
            InferMBLayoutFromInputsForStandardCase();
            InferImageDimsFromInputs(); 
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(1); 
        }
    };

    template class ScaleNode<float>; 
    template class ScaleNode<double>;

    // -----------------------------------------------------------------------
    // NegateNode (input)
    // computes the negative of its input
    // -----------------------------------------------------------------------

    template<class ElemType>
    class NegateNode : public ComputationNode<ElemType>, public NumInputs<1>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"Negate"; }
    public:
        NegateNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t /*inputIndex*/, const FrameRange & frameRange) override
        {
            Inputs(0)->GradientSlice(frameRange) -= GradientSlice(frameRange);
        }

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override 
        {
            ValueSlice(frameRange).AssignDifferenceOf(0, Inputs(0)->ValueSlice(frameRange));
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            ValidateUnaryMap(isFinalValidationPass);
        }
    };

    template class NegateNode<float>; 
    template class NegateNode<double>;

    // -----------------------------------------------------------------------
    // TimesNode (A, B)
    // right operand and output can have MB layout, while left operand cannot
    // -----------------------------------------------------------------------

    template<class ElemType>
    class TimesNode : public ComputationNode<ElemType>, public NumInputs<2>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"Times"; }
    public:
        TimesNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange) override
        {
            if (inputIndex == 0)    // left derivative
            {
                // this potentially computes inner products over time, so we use the Masked- variants
                Matrix<ElemType> sliceOutputGrad = MaskedGradientSlice(frameRange);
                Matrix<ElemType> sliceInput1Value = Inputs(1)->MaskedValueSlice(frameRange);

                // currently we only support one combination when the input is sparse.
                if (sliceInput1Value.GetMatrixType() == SPARSE && Inputs(0)->GradientValues().GetMatrixType() == DENSE && sliceOutputGrad.GetMatrixType() == DENSE)
                    Inputs(0)->GradientValues().SwitchToMatrixType(SPARSE, MatrixFormat::matrixFormatSparseBlockCol, false);

                Matrix<ElemType>::MultiplyAndAdd(sliceOutputGrad, false, sliceInput1Value, true, Inputs(0)->GradientValues());
            }
            else                    // right derivative
            {
                Matrix<ElemType> sliceInput1Grad = Inputs(1)->GradientSlice(frameRange);
                Matrix<ElemType> sliceOutputGrad = GradientSlice(frameRange);

                Matrix<ElemType>::MultiplyAndAdd(Inputs(0)->FunctionValues(), true, sliceOutputGrad, false, sliceInput1Grad);
            }
        }

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override
        {
            size_t rows0 = Inputs(0)->GetNumRows(), cols1 = Inputs(1)->GetNumCols();
            VerifySize(rows0, cols1);

            // right operand and output can have MB layout, while left operand cannot
            Matrix<ElemType> sliceInput1Value = Inputs(1)->ValueSlice(frameRange);
            Matrix<ElemType> sliceOutputValue = ValueSliceToDense(frameRange, false); // Output of Times node will always be dense

#if DUMPOUTPUT
            Inputs(0)->FunctionValues().Print("TimesNode - Input0");
#endif
            sliceOutputValue.AssignProductOf(Inputs(0)->FunctionValues(), false, sliceInput1Value, false);
#if NANCHECK
            sliceOutputValue.HasNan("Times");
#endif
#if DUMPOUTPUT
            sliceOutputValue.Print("TimesNode");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);

            //support automatic dimension inference for learnable parameters
            size_t rows0 = Inputs(0)->GetNumRows(), cols0 = Inputs(0)->GetNumCols();
            size_t rows1 = Inputs(1)->GetNumRows(), cols1 = Inputs(1)->GetNumCols();

            if (isFinalValidationPass && (rows0 == 0 || (cols1 == 0 && !Inputs(1)->GetMBLayout())))
                RuntimeError("Times operation: Inputs(0)->GetNumRows() and Inputs(1)->GetNumCols() should not be 0 since it cannot be automatically inferred");

            // limited automatic dimension inference for *children*, useful for CNN since it can be hard to know the size of each input parameter without deep knowledge how CNN is implemented (padding, stride)
            // TODO: ^^ There must be a better solution. Maybe MBLayout as well?
            // TODO: use dynamic_pointer_cast
            // infer cols0 as rows1
            if (cols0 == 0 && !Inputs(0)->GetMBLayout() && rows1 != 0 && isFinalValidationPass)
                ValidateInferChildDims(0, rows0, rows1);

            // infer rows1 as cols0
            if (cols0 != 0 && rows1 == 0)
                ValidateInferChildDims(1, cols0, cols1);

            if (isFinalValidationPass && Inputs(1)->GetNumRows() != Inputs(0)->GetNumCols())
                LogicError("The inner matrix dimension in the %ls %ls operation does not match (%d vs. %d).", NodeName().c_str(), OperationName().c_str(), (int)Inputs(1)->GetNumRows(), (int)Inputs(0)->GetNumCols());
            Resize(rows0, cols1);

            if (isFinalValidationPass && Inputs(0)->HasMBLayout())
                InvalidArgument("%ls %ls operation requires the first factor to not be minibatch data (must not have an MBLayout).", NodeName().c_str(), OperationName().c_str());
            InferMBLayoutFromInputsForStandardCase();
            InferImageDimsFromInputs(); 
        }

        virtual void InferImageDimsFromInputs()  
        {
            InferImageDimsFromInput(1, false); //the second one is the input since it's columnwise

            //after multiplication the structure is lost
            m_outputImageLayout = ImageLayout(1, Inputs(0)->GetNumRows(), 1);
        }
    };

    template class TimesNode<float>; 
    template class TimesNode<double>;

    // -----------------------------------------------------------------------
    // TransposeTimesNode (A', B)
    // right operand and output can have MB layout, while left operand cannot
    // TODO: merge with TimesNode?
    // -----------------------------------------------------------------------

    template<class ElemType>
    class TransposeTimesNode : public ComputationNode<ElemType>, public NumInputs<2>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"TransposeTimes"; }
    public:
        TransposeTimesNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        //void ComputeInputPartialMap(const size_t inputIndex)
        //{
        //    if (inputIndex > 1)
        //        InvalidArgument("TransposeTimesNode operation only takes two inputs.");
        //
        //    if (inputIndex == 0)  //left derivative
        //    {
        //        ComputeInputPartialLeft(Inputs(1)->FunctionValues(), Inputs(0)->GradientValues(), GradientValues());
        //    }
        //    else  //right derivative
        //    {
        //        ComputeInputPartialRight(Inputs(0)->FunctionValues(), Inputs(1)->GradientValues(), GradientValues());
        //    }
        //}

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange) override
        {
            //if (frameRange.IsAllFrames()) { ComputeInputPartialMap(inputIndex); return; } // TODO: remove these one by one

            if (inputIndex == 0)  //left derivative
            {
                // this potentially computes inner products over time, so we use the Masked- variants
                Matrix<ElemType> sliceOutputGrad = MaskedGradientSlice(frameRange);
                Matrix<ElemType> sliceInput1Value = Inputs(1)->MaskedValueSlice(frameRange);

                ComputeInputPartialLeft(sliceInput1Value, Inputs(0)->GradientValues(), sliceOutputGrad);
            }
            else  //right derivative
            {
                Matrix<ElemType> sliceInput1Grad = Inputs(1)->GradientSlice(frameRange);
                Matrix<ElemType> sliceOutputGrad = GradientSlice(frameRange);

                ComputeInputPartialRight(Inputs(0)->FunctionValues(), sliceInput1Grad, sliceOutputGrad);
            }
        }

        /*TODO: merge with call site*/void ComputeInputPartialLeft(Matrix<ElemType>& inputFunctionValues, Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)
        {
#if DUMPOUTPUT
            gradientValues.Print("Gradient-in");
            inputGradientValues.Print("child Gradient-in/out");
            inputFunctionValues.Print("child Function values");
#endif
            //currently we only support one combination when the input is sparse.
            if (inputFunctionValues.GetMatrixType() == SPARSE && inputGradientValues.GetMatrixType() == DENSE && gradientValues.GetMatrixType() == DENSE)
                inputGradientValues.SwitchToMatrixType(SPARSE, MatrixFormat::matrixFormatSparseBlockCol, false);

            Matrix<ElemType>::MultiplyAndAdd(inputFunctionValues, false, gradientValues, true, inputGradientValues);


#if DUMPOUTPUT
            inputGradientValues.Print("child Gradient-out");
#endif
        }

        /*TODO: merge with call site*/void ComputeInputPartialRight(Matrix<ElemType>& inputFunctionValues, Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)
        {
#if DUMPOUTPUT
            gradientValues.Print("Gradient-in");
            inputGradientValues.Print("child Gradient-in/out");
            inputFunctionValues.Print("child Function values");
#endif
            Matrix<ElemType>::MultiplyAndAdd(inputFunctionValues, false, gradientValues, false, inputGradientValues);

#if DUMPOUTPUT
            inputGradientValues.Print("child Gradient-out");
#endif
        }

        //void EvaluateThisNodeMap()    // TODO: This is a stop-gap; in most cases, we should just be able to delete this (but need to review one by one)
        //{
        //    EvaluateThisNodeS(FunctionValues(), Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues());
        //}

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override
        {
            //if (frameRange.IsAllFrames()) { EvaluateThisNodeMap(); return; }
            Matrix<ElemType> sliceInput1Value = Inputs(1)->ValueSlice(frameRange);
            Matrix<ElemType> sliceOutputValue = ValueSlice(frameRange);

            //EvaluateThisNodeS(sliceOutputValue, Inputs(0)->FunctionValues(), sliceInput1Value);
            sliceOutputValue.AssignProductOf(Inputs(0)->FunctionValues(), true, sliceInput1Value, false);
        }

//        /*TODO: merge with call site*/void EvaluateThisNodeS(Matrix<ElemType>& functionValues, const Matrix<ElemType>& input0, const Matrix<ElemType>& input1)
//        {
//#if DUMPOUTPUT
//            input0.Print("TransposeTimesNode - Input0");
//#endif
//            functionValues.AssignProductOf(input0, true, input1, false);
//#if NANCHECK
//            functionValues.HasNan("TransposeTimes");
//#endif
//#if DUMPOUTPUT
//            functionValues.Print("TransposeTimes");
//#endif
//        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);

            //support automatic dimension inference for learnable parameters
            size_t rows0 = Inputs(0)->GetNumRows(), cols0 = Inputs(0)->GetNumCols();
            size_t rows1 = Inputs(1)->GetNumRows(), cols1 = Inputs(1)->GetNumCols();

            if (isFinalValidationPass && (rows0 == 0 || (!Inputs(1)->HasMBLayout() && cols1 == 0)))
                RuntimeError("TransposeTimes operation: Inputs(0)->GetNumRows() and Inputs(1)->GetNumCols() should not be 0 since it cannot be automatically inferred");

            if (cols0 == 0 && rows1 != 0 && isFinalValidationPass)
                ValidateInferChildDims(0, rows0, rows1);

            if (cols0 != 0 && rows1 == 0)
                ValidateInferChildDims(1, cols0, cols1);

            //cols0 and rows1 may have been changed so don't use them in the following check
            if (isFinalValidationPass && Inputs(1)->GetNumRows() != Inputs(0)->GetNumRows())
                LogicError("The Matrix dimension in the TransposeTimes operation does not match.");

            Resize(cols0, cols1);
            InferMBLayoutFromInputsForStandardCase();   // TODO: what does the MBLayout mean in the context of TransposeTimes? Can the left arg have an MBLayout?
            InferImageDimsFromInputs();
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(1, false); //the second one is the input since it's column wize

            //after multiplication the structure is lost
            m_outputImageLayout = ImageLayout(1, Inputs(0)->GetNumRows(), 1);
        }
    };

    template class TransposeTimesNode<float>;
    template class TransposeTimesNode<double>;

    // -----------------------------------------------------------------------
    // ElementTimesNode (factor1, factor2)
    // -----------------------------------------------------------------------

    template<class ElemType>
    class ElementTimesNode : public ComputationNode<ElemType>, public NumInputs<2>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"ElementTimes"; }
    public:
        ElementTimesNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        //void ComputeInputPartialMap(const size_t inputIndex)  
        //{
        //    ComputeInputPartialS(Inputs(1-inputIndex)->FunctionValues(), Inputs(inputIndex)->GradientValues(), GradientValues());
        //}

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange) override
        {
            //if (frameRange.IsAllFrames()) { ComputeInputPartialMap(inputIndex); return; } // TODO: remove these one by one
            Matrix<ElemType> sliceInput0Grad = Inputs(inputIndex)->GradientSlice(frameRange);
            Matrix<ElemType> sliceOutputGrad = GradientSlice(frameRange);
            Matrix<ElemType> sliceInput1Value = Inputs(1-inputIndex)->ValueSlice(frameRange);

            //ComputeInputPartialS(sliceInput1Value, sliceInput0Grad, sliceOutputGrad);
            // depending on inputIndex, all the input variables change meaning
            // inputIndex == 0 (left) -  inputGradientValues[0], inputFunctionValues[1]
            // inputIndex == 1 (right) - inputGradientValues[1], inputFunctionValues[0]
            sliceInput0Grad.AddElementProductOf(sliceOutputGrad, sliceInput1Value);
        }

        // depending on inputIndex, all the input variables change meaning
        // inputIndex == 0 (left) -  inputGradientValues[0], inputFunctionValues[1]
        // inputIndex == 1 (right) - inputGradientValues[1], inputFunctionValues[0]
        ///*TODO: merge with call site*/void ComputeInputPartialS(Matrix<ElemType>& inputFunctionValues, Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)  
        //{
        //    inputGradientValues.AddElementProductOf(gradientValues, inputFunctionValues);
        //}

        //void EvaluateThisNodeMap()    // TODO: This is a stop-gap; in most cases, we should just be able to delete this (but need to review one by one)  
        //{
        //    EvaluateThisNodeS(FunctionValues(), Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues());
        //}

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override  
        {
            //if (frameRange.IsAllFrames()) { EvaluateThisNodeMap(); return; }
            Matrix<ElemType> sliceInput0Value = Inputs(0)->ValueSlice(frameRange);
            Matrix<ElemType> sliceInput1Value = Inputs(1)->ValueSlice(frameRange);
            Matrix<ElemType> sliceOutputValue = ValueSlice(frameRange);

            //EvaluateThisNodeS(sliceOutputValue, sliceInput0Value, sliceInput1Value);
            sliceOutputValue.AssignElementProductOf(sliceInput0Value, sliceInput1Value);
        }

        ///*TODO: merge with call site*/void EvaluateThisNodeS(Matrix<ElemType>& functionValues, const Matrix<ElemType>& input0, const Matrix<ElemType>& input1)  
        //{
        //    functionValues.AssignElementProductOf(input0, input1);
        //}

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            ValidateBinaryZip(isFinalValidationPass, false/*allowMultiple*/);
        }

        virtual void InferImageDimsFromInputs()
        {
            if (IsChildAnImage(0))  // if conflict, give priority to child 0
                InferImageDimsFromInput(0);
            else
                InferImageDimsFromInput(1);
        }
    };

    template class ElementTimesNode<float>; 
    template class ElementTimesNode<double>;

    // -----------------------------------------------------------------------
    // RowElementTimesNode (left, right)  --TODO: what are left and right?
    // -----------------------------------------------------------------------

    template<class ElemType>
    class RowElementTimesNode : public ComputationNode<ElemType>, public NumInputs<2>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"RowElementTimes"; }
    public:
        RowElementTimesNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        void ComputeInputPartialMap(const size_t inputIndex)
        {
            if (inputIndex > 1)
                InvalidArgument("RowElementTimes operation only takes two inputs.");

            if (inputIndex == 0)
            {
                ComputeInputPartialLeftS(Inputs(1)->FunctionValues(), Inputs(0)->GradientValues(), GradientValues(), *m_tempMatrix);
            }
            else
            {
                ComputeInputPartialRightS(Inputs(0)->FunctionValues(), Inputs(1)->GradientValues(), GradientValues(), *m_tempMatrix);
            }
        }

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange) override
        {
            if (frameRange.IsAllFrames()) { ComputeInputPartialMap(inputIndex); return; } // TODO: remove these one by one
            Matrix<ElemType> sliceInput0Grad = Inputs(inputIndex)->GradientSlice(frameRange);
            Matrix<ElemType> sliceOutputGrad = GradientSlice(frameRange);

            Matrix<ElemType> sliceInput1Value = Inputs(1 - inputIndex)->ValueSlice(frameRange);

            if (inputIndex == 0)
            {
                ComputeInputPartialLeftS(sliceInput1Value, sliceInput0Grad, sliceOutputGrad, *m_tempMatrix);
            }
            else
            {
                ComputeInputPartialRightS(sliceInput1Value, sliceInput0Grad, sliceOutputGrad, *m_tempMatrix);
            }
        }

        //left (input 0) is a matrix
        /*TODO: merge with call site*/void ComputeInputPartialLeftS(Matrix<ElemType>& input1FunctionValues,
            Matrix<ElemType>& input0GradientValues, 
            const Matrix<ElemType>& gradientValues, 
            Matrix<ElemType>& tempMatrix)
        {
            tempMatrix.SetValue(gradientValues);
            tempMatrix.RowElementMultiplyWith(input1FunctionValues);
            input0GradientValues += tempMatrix;

#if NANCHECK
            input0GradientValues.HasNan("RowElementTimes");
#endif
        }

        //right (input 1) is a row vector
        /*TODO: merge with call site*/void ComputeInputPartialRightS(Matrix<ElemType>& input0FunctionValues, 
            Matrix<ElemType>& input1GradientValues, 
            const Matrix<ElemType>& gradientValues, 
            Matrix<ElemType>& tempMatrix)
        {
            tempMatrix.AssignInnerProductOf(gradientValues, input0FunctionValues, true);
            input1GradientValues += tempMatrix;

#if NANCHECK
            input1GradientValues.HasNan("RowElementTimes");
#endif
        }
        void EvaluateThisNodeMap()    // TODO: This is a stop-gap; in most cases, we should just be able to delete this (but need to review one by one)
        {
            EvaluateThisNodeS(FunctionValues(), Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues());
        }

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override
        {
            //if (frameRange.IsAllFrames()) { EvaluateThisNodeMap(); return; }
            Matrix<ElemType> sliceInput0Value = Inputs(0)->ValueSlice(frameRange);
            Matrix<ElemType> sliceInput1Value = Inputs(1)->ValueSlice(frameRange);
            Matrix<ElemType> sliceOutputValue = ValueSlice(frameRange);

            EvaluateThisNodeS(sliceOutputValue, sliceInput0Value, sliceInput1Value);
        }

        /*TODO: merge with call site*/void EvaluateThisNodeS(Matrix<ElemType>& functionValues, const Matrix<ElemType>& input0, const Matrix<ElemType>& input1)
        {
            functionValues.SetValue(input0);
            functionValues.RowElementMultiplyWith(input1);

#if NANCHECK
            functionValues.HasNan("RowElementTimes");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);

            size_t rows0 = Inputs(0)->GetNumRows(), cols0 = Inputs(0)->GetNumCols();
            size_t rows1 = Inputs(1)->GetNumRows(), cols1 = Inputs(1)->GetNumCols(); rows0;
            if (isFinalValidationPass && cols0 != cols1 || rows1 != 1)
                LogicError("RowElementTimes: Either the second operand is not a row vector or the number of columns of operands does not match.");

            Resize(Inputs(0));
            InferMBLayoutFromInputsForStandardCase();
            InferImageDimsFromInputs();
        }

        virtual void InferImageDimsFromInputs()
        {
            // input 0 is the matrix and input 1 is a row vector
            InferImageDimsFromInput(0);
        }

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId)
        {
            Base::MoveMatricesToDevice(deviceId);
            m_tempMatrix->TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
        }

        //request matrices that are needed for gradient computation
        virtual void RequestMatricesBeforeGradientComp(MatrixPool& matrixPool)
        {
            Base::RequestMatricesBeforeGradientComp(matrixPool);
            RequestMatrixFromPool(m_tempMatrix, matrixPool);
        }

        //release gradient and temp matrices that no longer needed after all the children's gradients are computed.
        virtual void ReleaseMatricesAfterGradientComp(MatrixPool& matrixPool)
        {
            Base::ReleaseMatricesAfterGradientComp(matrixPool);
            ReleaseMatrixToPool(m_tempMatrix, matrixPool);
        }

    private:
        shared_ptr<Matrix<ElemType>> m_tempMatrix;
    };

    template class RowElementTimesNode<float>;
    template class RowElementTimesNode<double>;

    // -----------------------------------------------------------------------
    // ColumnElementTimesNode (left, right)  --TODO: what are left and right?
    // -----------------------------------------------------------------------

    template<class ElemType>
    class ColumnElementTimesNode : public ComputationNode<ElemType>, public NumInputs<2>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"ColumnElementTimes"; }
    public:
        ColumnElementTimesNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        void ComputeInputPartialMap(const size_t inputIndex)
        {
            if (inputIndex > 1)
                InvalidArgument("ColumnElementTimes operation only takes two inputs.");

            if (inputIndex == 0)
            {
                ComputeInputPartialLeftS(Inputs(1)->FunctionValues(), Inputs(0)->GradientValues(), GradientValues(), *m_tempMatrix);
            }
            else
            {
                ComputeInputPartialRightS(Inputs(0)->FunctionValues(), Inputs(1)->GradientValues(), GradientValues(), *m_tempMatrix);
            }
        }

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange) override
        {
            if (frameRange.IsAllFrames()) { ComputeInputPartialMap(inputIndex); return; } // TODO: remove these one by one
            Matrix<ElemType> sliceOutputGrad = GradientSlice(frameRange);

            if (inputIndex == 0)
            {
                Matrix<ElemType> sliceInput0Grad = Inputs(0)->GradientSlice(frameRange);

                ComputeInputPartialLeftS(Inputs(1)->FunctionValues(), sliceInput0Grad, sliceOutputGrad, *m_tempMatrix);
            }
            else
            {
                Matrix<ElemType> sliceInput0Value = Inputs(0)->ValueSlice(frameRange);
                ComputeInputPartialRightS(sliceInput0Value, Inputs(1)->GradientValues(), sliceOutputGrad, *m_tempMatrix);
            }
        }

        //left (input 0) is a matrix
        /*TODO: merge with call site*/void ComputeInputPartialLeftS(Matrix<ElemType>& input1FunctionValues,
            Matrix<ElemType>& input0GradientValues,
            const Matrix<ElemType>& gradientValues,
            Matrix<ElemType>& tempMatrix)
        {
            tempMatrix.SetValue(gradientValues);
            tempMatrix.ColumnElementMultiplyWith(input1FunctionValues);
            input0GradientValues += tempMatrix;

#if NANCHECK
            input0GradientValues.HasNan("ColumnElementTimes");
#endif
        }

        //right (input 1) is a col vector
        /*TODO: merge with call site*/void ComputeInputPartialRightS(Matrix<ElemType>& input0FunctionValues,
            Matrix<ElemType>& input1GradientValues,
            const Matrix<ElemType>& gradientValues,
            Matrix<ElemType>& tempMatrix)
        {
            tempMatrix.AssignInnerProductOf(gradientValues, input0FunctionValues, false);
            input1GradientValues += tempMatrix;

#if NANCHECK
            input1GradientValues.HasNan("ColumnElementTimes");
#endif
        }
        void EvaluateThisNodeMap()    // TODO: This is a stop-gap; in most cases, we should just be able to delete this (but need to review one by one)
        {
            EvaluateThisNodeS(FunctionValues(), Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues());
        }

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override
        {
            //if (frameRange.IsAllFrames()) { EvaluateThisNodeMap(); return; }
            Matrix<ElemType> sliceInput0Value = Inputs(0)->ValueSlice(frameRange);
            Matrix<ElemType> sliceOutputValue = ValueSlice(frameRange);

            EvaluateThisNodeS(sliceOutputValue, sliceInput0Value, Inputs(1)->FunctionValues());
        }

        /*TODO: merge with call site*/void EvaluateThisNodeS(Matrix<ElemType>& functionValues, const Matrix<ElemType>& input0, const Matrix<ElemType>& input1)
        {
            functionValues.SetValue(input0);
            functionValues.ColumnElementMultiplyWith(input1);

#if NANCHECK
            functionValues.HasNan("ColumnElementTimes");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);

            //derive number of rows if possible
            for (size_t index = 0; index < 2; index++)
            {
                size_t rows = Inputs(index)->GetNumRows() == 0 ? Inputs(1 - index)->GetNumRows() : Inputs(index)->GetNumRows();
                size_t cols = Inputs(index)->GetNumCols() == 0 ? Inputs(1 - index)->GetNumCols() : Inputs(index)->GetNumCols();
                ValidateInferChildDims(index, rows, cols);
            }

            size_t rows0 = Inputs(0)->GetNumRows(), cols0 = Inputs(0)->GetNumCols();
            size_t rows1 = Inputs(1)->GetNumRows(), cols1 = Inputs(1)->GetNumCols(); cols0;
            if (isFinalValidationPass && (rows0 != rows1 || cols1 != 1))
                LogicError("ColumnElementTimes: Either the second operand is not a column vector or the number of rows of operands does not match.");

            Resize(Inputs(0));
            InferMBLayoutFromInputsForStandardCase();
            InferImageDimsFromInputs();
        }

        virtual void InferImageDimsFromInputs()
        {
            // input 0 is a matrix and input 1 is a column vector
            InferImageDimsFromInput(0);
        }

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId)
        {
            Base::MoveMatricesToDevice(deviceId);
            m_tempMatrix->TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
        }

        //request matrices that are needed for gradient computation
        virtual void RequestMatricesBeforeGradientComp(MatrixPool& matrixPool)
        {
            Base::RequestMatricesBeforeGradientComp(matrixPool);
            RequestMatrixFromPool(m_tempMatrix, matrixPool);
        }

        //release gradient and temp matrices that no longer needed after all the children's gradients are computed.
        virtual void ReleaseMatricesAfterGradientComp(MatrixPool& matrixPool)
        {
            Base::ReleaseMatricesAfterGradientComp(matrixPool);
            ReleaseMatrixToPool(m_tempMatrix, matrixPool);
        }

    private:
        shared_ptr<Matrix<ElemType>> m_tempMatrix;
    };

    template class ColumnElementTimesNode<float>;
    template class ColumnElementTimesNode<double>;

    // -----------------------------------------------------------------------
    // DiagTimesNode (vector representing the diagonal of a square matrix, data)
    // -----------------------------------------------------------------------

    template<class ElemType>
    class DiagTimesNode : public ComputationNode<ElemType>, public NumInputs<2>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"DiagTimes"; }
    public:
        DiagTimesNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        //void ComputeInputPartialMap(const size_t inputIndex)
        //{
        //    if (inputIndex == 0)  //left derivative
        //        ComputeInputPartialLeft(*m_innerproduct, Inputs(1)->FunctionValues(), Inputs(0)->GradientValues(), GradientValues());
        //    else  //right derivative
        //        ComputeInputPartialRight(*m_rightGradient, Inputs(0)->FunctionValues(), Inputs(1)->GradientValues(), GradientValues());
        //}

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange) override
        {
            //if (frameRange.IsAllFrames()) { ComputeInputPartialMap(inputIndex); return; } // TODO: remove these one by one

            // left parameter (diag matix cannot be sliced)
            Matrix<ElemType> sliceOutputGrad = GradientSlice(frameRange);

            if (inputIndex == 0)    // left derivative
            {
                Matrix<ElemType> sliceInput1Value = Inputs(1)->ValueSlice(frameRange);
                ComputeInputPartialLeft(*m_innerproduct, sliceInput1Value, Inputs(0)->GradientValues(), sliceOutputGrad);
            }
            else                    // right derivative
            {
                Matrix<ElemType> sliceInput1Grad = Inputs(1)->GradientSlice(frameRange);
                ComputeInputPartialRight(*m_rightGradient, Inputs(0)->FunctionValues(), sliceInput1Grad, sliceOutputGrad);
            }
        }

        /*TODO: merge with call site*/void ComputeInputPartialLeft(Matrix<ElemType>& temp, const Matrix<ElemType>& inputFunctionValues, Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)  
        {
            temp.AssignInnerProductOf(gradientValues, inputFunctionValues, false);
            inputGradientValues += temp;
        }

        /*TODO: merge with call site*/void ComputeInputPartialRight(Matrix<ElemType>& temp, const Matrix<ElemType>& inputFunctionValues, Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)  
        {
            temp.SetValue(gradientValues);
            temp.ColumnElementMultiplyWith(inputFunctionValues);
            inputGradientValues += temp;
        }

        //void EvaluateThisNodeMap()    // TODO: This is a stop-gap; in most cases, we should just be able to delete this (but need to review one by one)  
        //{
        //    EvaluateThisNodeS(FunctionValues(), Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues()); 
        //}

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override  
        {
            //if (frameRange.IsAllFrames()) { EvaluateThisNodeMap(); return; }
            Matrix<ElemType> sliceInput1Value = Inputs(1)->ValueSlice(frameRange);
            Matrix<ElemType> sliceOutputValue = ValueSlice(frameRange);

            //EvaluateThisNodeS(sliceOutputValue, Inputs(0)->FunctionValues(), sliceInput1Value); 
            sliceOutputValue.SetValue(sliceInput1Value);
            sliceOutputValue.ColumnElementMultiplyWith(Inputs(0)->FunctionValues());
        }

        ///*TODO: merge with call site*/void EvaluateThisNodeS(Matrix<ElemType>& functionValues, const Matrix<ElemType>& inputFunctionValues0, const Matrix<ElemType>& inputFunctionValues1)  
        //{
        //    functionValues.SetValue(inputFunctionValues1);
        //    functionValues.ColumnElementMultiplyWith(inputFunctionValues0);
        //}

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);

            //if dimension not specified we assume two operands' dimensions should match
            if (Inputs(0)->GetNumRows() == 0 && Inputs(1)->GetNumRows() != 0)
                ValidateInferChildDims(0, Inputs(1)->GetNumRows(), 1);

            if (Inputs(0)->GetNumRows() != 0 && Inputs(1)->GetNumRows() == 0)
                ValidateInferChildDims(1, Inputs(0)->GetNumRows(), Inputs(1)->GetNumCols());

            if (isFinalValidationPass)
            {
                if (Inputs(1)->GetNumRows() != Inputs(0)->GetNumRows())
                    LogicError("The Matrix dimension in the DiagTimes operation does not match.");

                if (Inputs(0)->GetNumCols() != 1)
                    LogicError("The first matrix should be a vector representing the diagonal of a square matrix in the DiagTimes operation.");
            }

            Resize(Inputs(0)->GetNumRows(), Inputs(1)->GetNumCols());
            //m_innerproduct.Resize(Inputs(0)->GetNumRows(), Inputs(1)->GetNumCols());
            //m_rightGradient.Resize(Inputs(0)->GetNumRows(), Inputs(1)->GetNumCols());

            InferMBLayoutFromInputsForStandardCase();
            InferImageDimsFromInputs(); 
        }

        virtual void InferImageDimsFromInputs() //this is element wise scaling, so based on child 1
        {
            InferImageDimsFromInput(1);
        }

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId)
        {
            Base::MoveMatricesToDevice(deviceId);
            m_innerproduct->TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_rightGradient->TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
        }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<DiagTimesNode<ElemType>>(nodeP);
                *node->m_innerproduct = *m_innerproduct;
                *node->m_rightGradient = *m_rightGradient;
            }
        }
        //request matrices that are needed for gradient computation
        virtual void RequestMatricesBeforeGradientComp(MatrixPool& matrixPool)
        {
            Base::RequestMatricesBeforeGradientComp(matrixPool);
            RequestMatrixFromPool(m_innerproduct, matrixPool);
            RequestMatrixFromPool(m_rightGradient, matrixPool);
        }

        //release gradient and temp matrices that no longer needed after all the children's gradients are computed.
        virtual void ReleaseMatricesAfterGradientComp(MatrixPool& matrixPool)
        {
            Base::ReleaseMatricesAfterGradientComp(matrixPool);
            ReleaseMatrixToPool(m_innerproduct, matrixPool);
            ReleaseMatrixToPool(m_rightGradient, matrixPool);
        }
private:
        shared_ptr<Matrix<ElemType>> m_innerproduct;
        shared_ptr<Matrix<ElemType>> m_rightGradient;
    };

    template class DiagTimesNode<float>; 
    template class DiagTimesNode<double>;

    // -----------------------------------------------------------------------
    // SumElementsNode (input)
    // sums up all elements in the input
    // -----------------------------------------------------------------------

    template<class ElemType>
    class SumElementsNode : public ComputationNode<ElemType>, public NumInputs<1>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"SumElements"; }
    public:
        SumElementsNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        //void ComputeInputPartialMap(const size_t inputIndex)
        //{
        //    assert(inputIndex == 0); inputIndex;
        //    ComputeInputPartialS(Inputs(0)->GradientValues(), GradientValues());
        //}

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t /*inputIndex*/, const FrameRange & frameRange) override
        {
            //if (frameRange.IsAllFrames()) { ComputeInputPartialMap(inputIndex); return; } // TODO: remove these one by one

            Matrix<ElemType> sliceInputGrad = Inputs(0)->GradientSlice(frameRange);
            Matrix<ElemType> sliceOutputGrad = GradientSlice(frameRange);

            //ComputeInputPartialS(sliceInputGrad, sliceOutputGrad);
            sliceInputGrad += sliceOutputGrad; // here the assumption is that gradientValues are 1x1 matrix
        }

        ///*TODO: merge with call site*/void ComputeInputPartialS(Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)  
        //{
        //    inputGradientValues += gradientValues; //here the assumption is that gradientValues are 1x1 matrix
        //}

        //void EvaluateThisNodeMap()    // TODO: This is a stop-gap; in most cases, we should just be able to delete this (but need to review one by one)  
        //{
        //    EvaluateThisNodeS(FunctionValues(), Inputs(0)->FunctionValues());
        //}

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override
        {
            //if (frameRange.IsAllFrames()) { EvaluateThisNodeMap(); return; }
            // since we are reducing over frames, we must mask gaps to zero
            Matrix<ElemType> sliceInputValue = Inputs(0)->MaskedValueSlice(frameRange);
            Matrix<ElemType> sliceOutputValue = MaskedValueSlice(frameRange);

            //EvaluateThisNodeS(sliceOutputValue, sliceInputValue);
            sliceOutputValue.AssignSumOfElements(sliceInputValue);
        }

        ///*TODO: merge with call site*/void EvaluateThisNodeS(Matrix<ElemType>& functionValues, const Matrix<ElemType>& inputFunctionValues)  
        //{
        //    functionValues.AssignSumOfElements(inputFunctionValues);
        //}

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);

            Resize(1, 1);
            m_pMBLayout = nullptr;    // this node does not hold mini-batch data
            InferImageDimsFromInputs(); 
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, false);

            m_outputImageLayout = ImageLayout();
        }
    };

    template class SumElementsNode<float>; 
    template class SumElementsNode<double>;

    // -----------------------------------------------------------------------
    // SumColumnElementsNode (input)
    // sums up each column of the input
    // -----------------------------------------------------------------------

    template<class ElemType>
    class SumColumnElementsNode : public ComputationNode<ElemType>, public NumInputs<1>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"SumColumnElements"; }
    public:
        SumColumnElementsNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        //void ComputeInputPartialMap(const size_t inputIndex)
        //{
        //    assert(inputIndex == 0); inputIndex;
        //    ComputeInputPartialS(Inputs(0)->GradientValues(), GradientValues());
        //}

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t /*inputIndex*/, const FrameRange & frameRange) override
        {
            //if (frameRange.IsAllFrames()) { ComputeInputPartialMap(inputIndex); return; } // TODO: remove these one by one

            Matrix<ElemType> sliceInputGrad = Inputs(0)->GradientSlice(frameRange);
            Matrix<ElemType> sliceOutputGrad = GradientSlice(frameRange);

            //ComputeInputPartialS(sliceInputGrad, sliceOutputGrad);
            sliceInputGrad += sliceOutputGrad; // here the assumption is that gradientValues is a row vector
        }

        ///*TODO: merge with call site*/void ComputeInputPartialS(Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)
        //{
        //    inputGradientValues += gradientValues; //here the assumption is that gradientValues is a row vector
        //}

        //void EvaluateThisNodeMap()    // TODO: This is a stop-gap; in most cases, we should just be able to delete this (but need to review one by one)
        //{
        //    EvaluateThisNodeS(FunctionValues(), Inputs(0)->FunctionValues());
        //}

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override
        {
            //if (frameRange.IsAllFrames()) { EvaluateThisNodeMap(); return; }
            Matrix<ElemType> sliceInputValue = Inputs(0)->ValueSlice(frameRange);
            Matrix<ElemType> sliceOutputValue = ValueSlice(frameRange);

            //EvaluateThisNodeS(sliceOutputValue, sliceInputValue);
            Matrix<ElemType>::VectorSum(sliceInputValue, sliceOutputValue, true);
        }

        ///*TODO: merge with call site*/void EvaluateThisNodeS(Matrix<ElemType>& functionValues, const Matrix<ElemType>& inputFunctionValues)
        //{
        //    Matrix<ElemType>::VectorSum(inputFunctionValues, functionValues, true);
        //}

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);

            Resize(1, Inputs(0)->GetNumCols());
            InferMBLayoutFromInputsForStandardCase();
            InferImageDimsFromInputs();
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, false);

            m_outputImageLayout = ImageLayout();
        }
    };

    template class SumColumnElementsNode<float>;
    template class SumColumnElementsNode<double>;

    // -----------------------------------------------------------------------
    // TransposeNode (input matrix)
    // -----------------------------------------------------------------------

    template<class ElemType>
    class TransposeNode : public ComputationNodeNonLooping/*ComputationNode*/<ElemType>, public NumInputs<1>
    {
        typedef ComputationNodeNonLooping<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"Transpose"; }

    public:
        TransposeNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        virtual void ComputeInputPartialNonLooping(size_t /*inputIndex*/) override
        {
            Matrix<ElemType>& inputGradientValues = Inputs(0)->GradientValues();
            const Matrix<ElemType>& gradientValues = GradientValues();
#if DUMPOUTPUT
            gradientValues.Print("Gradient-in");
            inputGradientValues.Print("child Gradient-in/out");
            inputFunctionValues.Print("child Function values");
#endif
            const Matrix<ElemType>& ones = ConstOnes(inputGradientValues.GetNumRows(), inputGradientValues.GetNumRows(), inputGradientValues.GetDeviceId());
            Matrix<ElemType>::MultiplyAndAdd(ones, false, gradientValues, true, inputGradientValues);
#if DUMPOUTPUT
            inputGradientValues.Print("child Gradient-out");
#endif
        }

        virtual void /*ComputationNodeNonLooping::*/EvaluateThisNodeNonLooping() override
        {
#if DUMPOUTPUT
            Inputs(0)->FunctionValues().Print("TransposeNode- Input0");
#endif
            FunctionValues().AssignTransposeOf(Inputs(0)->FunctionValues());
#if NANCHECK
            FunctionValues().HasNan("Transpose");
#endif
#if DUMPOUTPUT
            FunctionValues().Print("TransposeNode");
#endif
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);

            size_t rows0 = Inputs(0)->GetNumRows(), cols0 = Inputs(0)->GetNumCols();

            Resize(cols0, rows0);
            if (Inputs(0)->HasMBLayout())
                InvalidArgument("%ls %ls operation cannot operate on minibatch data (which have a layout)", NodeName().c_str(), OperationName().c_str());
            m_pMBLayout = nullptr;    // this node does not hold mini-batch data
            InferImageDimsFromInputs();
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, false); // the second one is the input since it's column wize

            // after transposition, the structure is lost
            m_outputImageLayout = ImageLayout(1, Inputs(0)->GetNumCols(), 1);
        }
    };

    template class TransposeNode<float>;
    template class TransposeNode<double>;

    // -----------------------------------------------------------------------
    // DiagonalNode -- extract diagonal elements of a matrix
    // -----------------------------------------------------------------------

    template<class ElemType>
    class DiagonalNode : public ComputationNode<ElemType>, public NumInputs<1>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"Diagonal"; }
    public:
        DiagonalNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<DiagonalNode<ElemType>>(nodeP);
            }
        }

        virtual void SaveToFile(File& fstream) const
        {
            Base::SaveToFile(fstream);
        }

        virtual void LoadFromFile(File& fstream, size_t modelVersion)
        {
            Base::LoadFromFile(fstream, modelVersion);
        }

        virtual void AttachInputs(const ComputationNodePtr singleInput)
        {
            m_children.resize(1);
            m_children[0] = singleInput;
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, true);

            m_outputImageLayout.width = 1;
            m_outputImageLayout.channels = 1;

            if (m_inputImageLayout.width * m_inputImageLayout.channels != 1)
                fprintf(stderr, "WARNING: Diagonal operation cannot inherit image size information from its child. Image size info is lost.\n");
        }

        virtual void PrintSelfBeforeValidation(bool allowNulls = false) const
        {
            fprintf(stderr, "\nValidating --> %ls = %ls", NodeName().c_str(), OperationName().c_str());

            if (!IsLeaf())
            {
                fprintf(stderr, "(");
                for (size_t i = 0; i < ChildrenSize(); i++)
                {
                    ComputationNodePtr child = Inputs(i);
                    if (i > 0)
                        fprintf(stderr, ", ");

                    if (child == nullptr)
                    {
                        if (allowNulls)
                        {
                            fprintf(stderr, "NULL");
                            continue;
                        }
                        RuntimeError("One of the children is missing.");
                    }

                    fprintf(stderr, "%ls[%lu, %lu]", child->NodeName().c_str(), child->FunctionValues().GetNumRows(), child->FunctionValues().GetNumCols());
                }

                fprintf(stderr, ")");
            }
        }

        virtual void Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);

            if (m_children.size() != 1)
                LogicError("Diagonal operation: Should have one input.");

            if (Inputs(0)->FunctionValues().GetNumElements() == 0)
                LogicError("Diagonal operation: The input node has 0 element.");

            size_t cols = Inputs(0)->FunctionValues().GetNumCols();

            FunctionValues().Resize(1, cols);
            InferMBLayoutFromInputsForStandardCase();
            InferImageDimsFromInputs();
        }

        virtual void EvaluateThisNode()
        {
            EvaluateThisNodeS(FunctionValues(), Inputs(0)->FunctionValues());
        }

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & /*frameRange*/)
        {
            NOT_IMPLEMENTED
        }

        static void WINAPI EvaluateThisNodeS(Matrix<ElemType>& functionValues, const Matrix<ElemType>& inputFunctionValues)
        {
            functionValues.Resize(1, inputFunctionValues.GetNumCols());
            inputFunctionValues.AssignDiagonalValuesTo(functionValues);
#if NANCHECK
            functionValues.HasNan("Diagonal");
#endif
        }

        void ComputeInputPartialMap(const size_t inputIndex)
        {
            if (inputIndex > 0)
                InvalidArgument("Diagonal operation only takes one input.");

            ComputeInputPartialS(Inputs(0)->GradientValues(), GradientValues());
        }

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange) override
        {
            if (frameRange.IsAllFrames()) { ComputeInputPartialMap(inputIndex); return; } // TODO: remove these one by one
            NOT_IMPLEMENTED
        }

        static void WINAPI ComputeInputPartialS(Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)
        {
            Matrix<ElemType> diag(gradientValues.GetNumRows(), gradientValues.GetNumCols(), gradientValues.GetDeviceId());
            diag = gradientValues;
            diag.Resize(gradientValues.GetNumCols(), 1);

            inputGradientValues.SetValue(0);
            inputGradientValues.SetDiagonalValue(diag);
        }

        virtual const Matrix<ElemType>& FunctionValues() const
        {
            return *m_functionValues;
        }
    };

    template class DiagonalNode<float>;
    template class DiagonalNode<double>;

    // -----------------------------------------------------------------------
    // CosDistanceNode (left, right)
    // -----------------------------------------------------------------------

    //The first matrix should be a vector regpresting the diagonal of a square matrix in the DiagTimes operation
    template<class ElemType>
    class CosDistanceNode : public ComputationNode<ElemType>, public NumInputs<2>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"CosDistance"; }
    public:
        CosDistanceNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        void ComputeInputPartialMap(const size_t inputIndex)
        {
            if (inputIndex > 1)
                InvalidArgument("CosDistance operation only takes two inputs.");

            if (inputIndex == 0)  //left derivative
            {
                ComputeInputPartialLeft(*m_invNorm0, *m_invNorm1, FunctionValues(), *m_temp, *m_rightTerm, *m_leftTerm, Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues(), GradientValues(), Inputs(inputIndex)->GradientValues());
            }
            else  //right derivative
            {
                ComputeInputPartialRight(*m_invNorm0, *m_invNorm1, FunctionValues(), *m_temp, *m_rightTerm, *m_leftTerm, Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues(), GradientValues(), Inputs(inputIndex)->GradientValues());
            }
        }

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange) override
        {
            if (frameRange.IsAllFrames()) { ComputeInputPartialMap(inputIndex); return; } // TODO: remove these one by one
            Matrix<ElemType> sliceInput0Value = Inputs(0)->ValueSlice(frameRange);
            Matrix<ElemType> sliceInput1Value = Inputs(1)->ValueSlice(frameRange);
            Matrix<ElemType> sliceOutputValue = ValueSlice(frameRange);
            Matrix<ElemType> sliceInputGrad = Inputs(inputIndex)->GradientSlice(frameRange);
            Matrix<ElemType> sliceOutputGrad = this->GradientSlice(frameRange);

            if (inputIndex == 0)  //left derivative
            {
                ComputeInputPartialLeft(*m_invNorm0, *m_invNorm1, sliceOutputValue, *m_temp, *m_rightTerm, *m_leftTerm, sliceInput0Value, sliceInput1Value, sliceOutputGrad, sliceInputGrad);
            }
            else  //right derivative
            {
                ComputeInputPartialRight(*m_invNorm0, *m_invNorm1, sliceOutputValue, *m_temp, *m_rightTerm, *m_leftTerm, sliceInput0Value, sliceInput1Value, sliceOutputGrad, sliceInputGrad);
            }
        }

        /*TODO: merge with call site*/void ComputeInputPartialLeft(const Matrix<ElemType>& invNorm0, const Matrix<ElemType>& invNorm1, const Matrix<ElemType>& functionValues, 
            Matrix<ElemType>& temp, Matrix<ElemType>& rightTerm, Matrix<ElemType>& leftTerm, // the temporary variables
            const Matrix<ElemType>& in0, const Matrix<ElemType>& in1, const Matrix<ElemType>& gradientValues,
            Matrix<ElemType>& inputGradientValues)
        {
            ComputeInputPartialS(0, invNorm0, invNorm1, functionValues, temp, rightTerm, leftTerm, in0, in1, gradientValues, inputGradientValues);
        }

        /*TODO: merge with call site*/void ComputeInputPartialRight(const Matrix<ElemType>& invNorm0, const Matrix<ElemType>& invNorm1, const Matrix<ElemType>& functionValues, 
            Matrix<ElemType>& temp, Matrix<ElemType>& rightTerm, Matrix<ElemType>& leftTerm, // the temporary variables
            const Matrix<ElemType>& in0, const Matrix<ElemType>& in1, const Matrix<ElemType>& gradientValues,
            Matrix<ElemType>& inputGradientValues)  
        {
            ComputeInputPartialS(1, invNorm0, invNorm1, functionValues, temp, rightTerm, leftTerm, in0, in1, gradientValues, inputGradientValues);  
        }

        // functionValues, invNorm0, invNorm1 - output from the EvaluateNode() method
        // temp, rightTerm, leftTerm - temporary matrices
        // in0, in1 - input functionValues from other nodes
        // inputGradientValues(x) - gradients to update, where x matches inputIndex
        /*TODO: merge with call site*/void ComputeInputPartialS(const size_t inputIndex, const Matrix<ElemType>& invNorm0, const Matrix<ElemType>& invNorm1, const Matrix<ElemType>& functionValues, 
            Matrix<ElemType>& temp, Matrix<ElemType>& rightTerm, Matrix<ElemType>& leftTerm, // the temporary variables
            const Matrix<ElemType>& in0, const Matrix<ElemType>& in1, const Matrix<ElemType>& gradientValues,
            Matrix<ElemType>& inputGradientValues)  
        {
            if (inputIndex == 0)  //left derivative
            {
                temp.AssignElementProductOf(invNorm0, invNorm0);
            }
            else  //right derivative
            {
                temp.AssignElementProductOf(invNorm1, invNorm1);
            }

            temp.ElementMultiplyWith(functionValues);
            rightTerm.SetValue(inputIndex?in1:in0);
            rightTerm.RowElementMultiplyWith(temp);

            temp.AssignElementProductOf(invNorm0, invNorm1);
            leftTerm.SetValue(inputIndex?in0:in1);
            leftTerm.RowElementMultiplyWith(temp);

            leftTerm -= rightTerm;
            leftTerm.RowElementMultiplyWith(gradientValues);
            inputGradientValues += leftTerm;
            
            //alternatively the above three lines can be replaced by
            //leftTerm.RowElementMultiplyWith(gradientValues);
            //rightTerm.RowElementMultiplyWith(gradientValues);
            //Matrix<ElemType>::AddScaledDifference(1, leftTerm, rightTerm, inputGradientValues);
        }

        void EvaluateThisNodeMap()    // TODO: This is a stop-gap; in most cases, we should just be able to delete this (but need to review one by one)  
        {
            EvaluateThisNodeS(*m_invNorm0, *m_invNorm1, FunctionValues(), Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues());  
        }

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override 
        {
            //if (frameRange.IsAllFrames()) { EvaluateThisNodeMap(); return; }
            Matrix<ElemType> sliceInput0Value = Inputs(0)->ValueSlice(frameRange);
            Matrix<ElemType> sliceInput1Value = Inputs(1)->ValueSlice(frameRange);
            Matrix<ElemType> sliceOutputValue = ValueSlice(frameRange);

            EvaluateThisNodeS(*m_invNorm0, *m_invNorm1, sliceOutputValue, sliceInput0Value, sliceInput1Value);  
        }

        /*TODO: merge with call site*/void EvaluateThisNodeS(Matrix<ElemType>& invNorm0, Matrix<ElemType>& invNorm1, Matrix<ElemType>& functionValues, Matrix<ElemType>& in0, Matrix<ElemType>& in1)  
        {
            invNorm0.AssignVectorNorm2Of(in0, true); // seems to modify input (in0)
            invNorm0.AssignElementInverseOf(invNorm0);

            invNorm1.AssignVectorNorm2Of(in1, true); // seems to modify the input (in1)
            invNorm1.AssignElementInverseOf(invNorm1);

            functionValues.AssignInnerProductOf(in0, in1, true);
            functionValues.ElementMultiplyWith(invNorm0);
            functionValues.ElementMultiplyWith(invNorm1);
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);
            ValidateInferBinaryChildrenDims();

#if 0
            size_t index = 0;
            {
                size_t rows = Inputs(index)->GetNumRows() == 0? Inputs(1-index)->GetNumRows() : Inputs(index)->GetNumRows();
                size_t cols = Inputs(index)->GetNumCols() == 0? Inputs(1-index)->GetNumCols() : Inputs(index)->GetNumCols();
                ValidateInferChildDims(index, rows, cols);
            }

            index = 1;
            {
                size_t rows = Inputs(index)->GetNumRows() == 0? Inputs(1-index)->GetNumRows() : Inputs(index)->GetNumRows();
                size_t cols = Inputs(index)->GetNumCols() == 0? Inputs(1-index)->GetNumCols() : Inputs(index)->GetNumCols();
                ValidateInferChildDims(index, rows, cols);
            }
#endif

#if 0
            if (isFinalValidationPass && (Inputs(1)->GetNumRows() != Inputs(0)->GetNumRows() || (HasMBLayout() && (Inputs(1)->GetNumCols() != Inputs(0)->GetNumCols()))))
                LogicError("The Matrix dimension in the CosDistance operation does not match.");
#endif

            Resize(1, Inputs(1)->GetNumCols());

            InferMBLayoutFromInputsForStandardCase();
            InferImageDimsFromInputs(); 
        }

        virtual void InferImageDimsFromInputs() 
        {
            InferImageDimsFromInput(0, false);

            m_outputImageLayout = ImageLayout();
        }

        virtual void MoveMatricesToDevice(const DEVICEID_TYPE deviceId)
        {
            Base::MoveMatricesToDevice(deviceId);
            m_invNorm0->TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_invNorm1->TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_leftTerm->TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_rightTerm->TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_temp->TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
        }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<CosDistanceNode<ElemType>>(nodeP);
                *node->m_invNorm0 = *m_invNorm0;
                *node->m_invNorm1 = *m_invNorm1;
                *node->m_leftTerm = *m_leftTerm;
                *node->m_rightTerm = *m_rightTerm;
                *node->m_temp = *m_temp;
            }
        }
        //request matrices needed to do node function value evaluation
        virtual void RequestMatricesBeforeEval(MatrixPool& matrixPool)
        {
            Base::RequestMatricesBeforeEval(matrixPool);
            RequestMatrixFromPool(m_invNorm0, matrixPool);
            RequestMatrixFromPool(m_invNorm1, matrixPool);
        }

        //request matrices that are needed for gradient computation
        virtual void RequestMatricesBeforeGradientComp(MatrixPool& matrixPool)
        {
            Base::RequestMatricesBeforeGradientComp(matrixPool);
            RequestMatrixFromPool(m_leftTerm, matrixPool);
            RequestMatrixFromPool(m_rightTerm, matrixPool);
            RequestMatrixFromPool(m_temp, matrixPool);
        }

        //release gradient and temp matrices that no longer needed after all the children's gradients are computed.
        virtual void ReleaseMatricesAfterGradientComp(MatrixPool& matrixPool)
        {
            Base::ReleaseMatricesAfterGradientComp(matrixPool);
            ReleaseMatrixToPool(m_invNorm0, matrixPool);
            ReleaseMatrixToPool(m_invNorm1, matrixPool);
            ReleaseMatrixToPool(m_leftTerm, matrixPool);
            ReleaseMatrixToPool(m_rightTerm, matrixPool);
            ReleaseMatrixToPool(m_temp, matrixPool);
        }
private:
        // invNorm nodes tranfer data between EvaluateThisNode and ComputeInputPartial
        shared_ptr<Matrix<ElemType>> m_invNorm0;
        shared_ptr<Matrix<ElemType>> m_invNorm1;
        // the rest are temporaries, values don't need to be maintained
        shared_ptr<Matrix<ElemType>> m_leftTerm;
        shared_ptr<Matrix<ElemType>> m_rightTerm;
        shared_ptr<Matrix<ElemType>> m_temp;
    };

    template class CosDistanceNode<float>; 
    template class CosDistanceNode<double>;

    // -----------------------------------------------------------------------
    // KhatriRaoProductNode (left, right)
    // -----------------------------------------------------------------------

    template<class ElemType>
    class KhatriRaoProductNode : public ComputationNode<ElemType>, public NumInputs<2>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"KhatriRaoProduct"; }
    public:
        KhatriRaoProductNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange) override
        {
            Matrix<ElemType> sliceOutputGrad = GradientSlice(frameRange);

            if (inputIndex == 0)  //left derivative
            {
                Matrix<ElemType> sliceInput0Grad = Inputs(0)->GradientSlice(frameRange);
                Matrix<ElemType> sliceInput1Value = Inputs(1)->ValueSlice(frameRange);

                //ComputeInputPartialLeft(sliceInput1Value, sliceInput0Grad, sliceOutputGrad); 
                sliceInput0Grad.AddColumnReshapeProductOf(sliceOutputGrad, sliceInput1Value, false);
            }
            else  //right derivative
            {
                Matrix<ElemType> sliceInput0Value = Inputs(0)->ValueSlice(frameRange);
                Matrix<ElemType> sliceInput1Grad = Inputs(1)->GradientSlice(frameRange);

                //ComputeInputPartialRight(sliceInput0Value, sliceInput1Grad, sliceOutputGrad); 
                sliceInput1Grad.AddColumnReshapeProductOf(sliceOutputGrad, sliceInput0Value, true);
            }
        }

        ///*TODO: merge with call site*/void ComputeInputPartialLeft(Matrix<ElemType>& childFunctionValues, Matrix<ElemType>& childGradientValues, const Matrix<ElemType>& gradientValues)
        //{
        //    childGradientValues.AddColumnReshapeProductOf(gradientValues, childFunctionValues, false);
        //}
        //
        ///*TODO: merge with call site*/void ComputeInputPartialRight(Matrix<ElemType>& childFunctionValues, Matrix<ElemType>& childGradientValues, const Matrix<ElemType>& gradientValues)  
        //{
        //    childGradientValues.AddColumnReshapeProductOf(gradientValues, childFunctionValues, true);
        //}

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override  
        {
            ValueSlice(frameRange).AssignKhatriRaoProductOf(Inputs(0)->ValueSlice(frameRange), Inputs(1)->ValueSlice(frameRange));
        }

        ///*TODO: merge with call site*/void EvaluateThisNodeS(Matrix<ElemType>& functionValues, Matrix<ElemType>& in0, Matrix<ElemType>& in1)  
        //{
        //    functionValues.AssignKhatriRaoProductOf(in0,in1);
        //}

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);

            //support automatic dimension inference for learnable parameters
            size_t rows0 = Inputs(0)->GetNumRows(), cols0 = Inputs(0)->GetNumCols();
            size_t rows1 = Inputs(1)->GetNumRows(), cols1 = Inputs(1)->GetNumCols();

            if (cols0 == 0 && cols1 != 0)
                ValidateInferChildDims(0, rows0, cols1);

            if (cols0 != 0 && cols1 == 0)
                ValidateInferChildDims(1, rows1, cols0);

            if (isFinalValidationPass && Inputs(1)->GetNumCols() != Inputs(0)->GetNumCols())
                LogicError("The Matrices should have same number of columns.");

            Resize(rows0 * rows1, Inputs(0)->GetNumCols());
            InferMBLayoutFromInputsForStandardCase();
            InferImageDimsFromInputs(); 
        }

        virtual void InferImageDimsFromInputs()  
        {
            //since it's symmetrical any one of the input may be the true input. 
            //since we dont' use the input image size info in the operation, the input part doesn't matter.
            InferImageDimsFromInput(1, false); 

            //after KhatriRaoProduct the structure is lost
            m_outputImageLayout = ImageLayout(1, m_functionValues->GetNumRows(), 1);
        }
    };

    template class KhatriRaoProductNode<float>; 
    template class KhatriRaoProductNode<double>;

    // -----------------------------------------------------------------------
    // CosDistanceWithNegativeSamplesNode (left, right, shift, neg)
    // -----------------------------------------------------------------------

    template<class ElemType>
    class CosDistanceWithNegativeSamplesNode : public ComputationNode<ElemType>, public NumInputs<4>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"CosDistanceWithNegativeSamples"; }
    public:
        CosDistanceWithNegativeSamplesNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name)
        { }

        void ComputeInputPartialMap(const size_t inputIndex)
        {
            if (inputIndex > 1)
                InvalidArgument("CosDistanceWithNegativeSamples operation only takes grdients on the first two inputs.");

            ComputeInputPartialS(inputIndex, *m_invNorm0, *m_invNorm1, FunctionValues(), *m_temp, *m_rightTerm, *m_leftTerm, *m_invNormSquare, Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues(), Inputs(2)->FunctionValues(), Inputs(3)->FunctionValues(), Inputs(inputIndex)->GradientValues(), GradientValues());
        }

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange) override
        {
            if (frameRange.IsAllFrames()) { ComputeInputPartialMap(inputIndex); return; } // TODO: remove these one by one
            Matrix<ElemType> sliceInput0Value = Inputs(0)->ValueSlice(frameRange);
            Matrix<ElemType> sliceInput1Value = Inputs(1)->ValueSlice(frameRange);
            Matrix<ElemType> sliceOutputValue = ValueSlice(frameRange);
            Matrix<ElemType> sliceInputGrad = Inputs(inputIndex)->GradientSlice(frameRange);
            Matrix<ElemType> sliceThisGrad = GradientSlice(frameRange);

            ComputeInputPartialS(inputIndex, *m_invNorm0, *m_invNorm1, sliceOutputValue, *m_temp, *m_rightTerm, *m_leftTerm, *m_invNormSquare, sliceInput0Value, sliceInput1Value, Inputs(2)->FunctionValues(), Inputs(3)->FunctionValues(), sliceInputGrad, sliceThisGrad);
        }

        // functionValues, invNorm0, invNorm1 - output from the EvaluateNode() method
        // temp, rightTerm, leftTerm - temporary matrices
        // in0, in1, in2, in3 - input functionValues from other nodes
        // inputGradientValues(x) - gradients to update, where x matches inputIndex
        /*TODO: merge with call site*/void ComputeInputPartialS(const size_t inputIndex, const Matrix<ElemType>& invNorm0, const Matrix<ElemType>& invNorm1, const Matrix<ElemType>& functionValues,
            Matrix<ElemType>& temp, Matrix<ElemType>& rightTerm, Matrix<ElemType>& leftTerm, Matrix<ElemType>& invNormSquare, // the temporary variables
            const Matrix<ElemType>& in0, const Matrix<ElemType>& in1, const Matrix<ElemType>& in2, const Matrix<ElemType>& in3,
            Matrix<ElemType>& inputGradientValues, Matrix<ElemType>& thisGradientValues)
        {
            size_t shift = (size_t)in2.Get00Element();
            size_t negNumber = (size_t)in3.Get00Element();
            size_t numCols = in0.GetNumCols(); // used in computing right child's graident

            if (inputIndex == 0) // left derivative
            {
                invNormSquare.AssignElementProductOf(invNorm0, invNorm0);

                for (long m = 0; m < negNumber + 1; m++)
                {
                    temp.GetARowByIndex(functionValues, m); // set this matrx to be the m-th row in functionValues
                    temp.ElementMultiplyWith(invNormSquare);

                    Matrix<ElemType>::ConductRowElementMultiplyWithShift(temp, in0, rightTerm, 0, true);

                    if (m == 0)
                    {
                        temp.AssignElementProductOf(invNorm0, invNorm1);

                        Matrix<ElemType>::ConductRowElementMultiplyWithShift(temp, in1, leftTerm, 0, true);
                    }
                    else
                    {
                        size_t currshift = m + shift - 1;  // for current line, how much should we shift

                        temp.AssignElementProductOfWithShift(invNorm0, invNorm1, currshift); // this is a row vector

                        Matrix<ElemType>::ConductRowElementMultiplyWithShift(temp, in1, leftTerm, currshift, true);
                    }

                    leftTerm = leftTerm - rightTerm;

                    temp.GetARowByIndex(thisGradientValues, m);

                    Matrix<ElemType>::ConductRowElementMultiplyWithShift(temp, leftTerm, rightTerm, 0, true);

                    inputGradientValues += rightTerm;
                }
            }
            else // right part
            {
                invNormSquare.AssignElementProductOf(invNorm1, invNorm1);  //this matrix should be save and unchanged. It should not be changed

                for (long m = 0; m < negNumber + 1; m++)
                {
                    temp.GetARowByIndex(functionValues, m); // set this matrx to be the m-th row in functionValues

                    if (m == 0) // this is the first line. computation should be symmetric
                    {
                        // the following is for the right part
                        temp.ElementMultiplyWith(invNormSquare);

                        Matrix<ElemType>::ConductRowElementMultiplyWithShift(temp, in1, rightTerm, 0, true);

                        // the following is for the left part
                        temp.AssignElementProductOf(invNorm0, invNorm1);

                        Matrix<ElemType>::ConductRowElementMultiplyWithShift(temp, in0, leftTerm, 0, true);

                        leftTerm = leftTerm - rightTerm;

                        temp.GetARowByIndex(thisGradientValues, m);

                        Matrix<ElemType>::ConductRowElementMultiplyWithShift(temp, leftTerm, rightTerm, 0, true);

                        inputGradientValues += rightTerm;
                    }
                    else // this requires shift
                    {
                        size_t currshift = (m + shift - 1) % numCols;
                        size_t reverseshift = numCols - currshift;

                        leftTerm.AssignElementProductOfWithShift(invNormSquare, temp, reverseshift);  //use leftTerm as a temp variable here

                        Matrix<ElemType>::ConductRowElementMultiplyWithShift(leftTerm, in1, rightTerm, 0, true);

                        temp.AssignElementProductOfWithShift(invNorm1, invNorm0, reverseshift);

                        Matrix<ElemType>::ConductRowElementMultiplyWithShift(temp, in0, leftTerm, reverseshift, true);

                        leftTerm = leftTerm - rightTerm;

                        temp.GetARowByIndex(thisGradientValues, m);

                        Matrix<ElemType>::ConductRowElementMultiplyWithShift(temp, leftTerm, rightTerm, reverseshift, false);

                        inputGradientValues += rightTerm;
                    }
                }
            }
        }

        void EvaluateThisNodeMap()    // TODO: This is a stop-gap; in most cases, we should just be able to delete this (but need to review one by one)
        {
            EvaluateThisNodeS(*m_invNorm0, *m_invNorm1, FunctionValues(), Inputs(0)->FunctionValues(), Inputs(1)->FunctionValues(), Inputs(2)->FunctionValues(), Inputs(3)->FunctionValues(), *m_leftTerm, *m_rightTerm);
        }

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override
        {
            //if (frameRange.IsAllFrames()) { EvaluateThisNodeMap(); return; }
            Matrix<ElemType> sliceInput0Value = Inputs(0)->ValueSlice(frameRange);
            Matrix<ElemType> sliceInput1Value = Inputs(1)->ValueSlice(frameRange);
            Matrix<ElemType> sliceOutputValue = ValueSlice(frameRange);

            EvaluateThisNodeS(*m_invNorm0, *m_invNorm1, sliceOutputValue, sliceInput0Value, sliceInput1Value, Inputs(2)->FunctionValues(), Inputs(3)->FunctionValues(), *m_leftTerm, *m_rightTerm);
        }

        /*TODO: merge with call site*/void EvaluateThisNodeS(Matrix<ElemType>& invNorm0, Matrix<ElemType>& invNorm1, Matrix<ElemType>& functionValues, Matrix<ElemType>& in0, Matrix<ElemType>& in1, Matrix<ElemType>& in2, Matrix<ElemType>& in3, Matrix<ElemType>& leftTermTemp, Matrix<ElemType>& rightTermTemp)
        {
            invNorm0.AssignVectorNorm2Of(in0, true); // seems to modify input (in0)
            invNorm0.AssignElementInverseOf(invNorm0);

            invNorm1.AssignVectorNorm2Of(in1, true); // seems to modify the input (in1)
            invNorm1.AssignElementInverseOf(invNorm1);

            size_t shift = (size_t)in2.Get00Element();
            size_t negNumber = (size_t)in3.Get00Element();

            // mutiply invNorm0 and invNorm1 with shift and neg. 
            // The result is a matrix of (numberneg+1, invNorm0.Cols)
            leftTermTemp.AssignElementProductOfWithShiftNeg(invNorm0, invNorm1, shift, negNumber);

            // compute the right values
            // Again, the ouput is a matrix of (negNumber+1, invNorm0.cols)
            rightTermTemp.AssignInnerProductOfWithShiftNeg(in0, in1, true, shift, negNumber);

            // compute the evaluation result matrix by multiply these two matrices, element by element
            // we get a (negNumber+1, n) matrix
            functionValues.AssignElementProductOf(leftTermTemp, rightTermTemp);
        }

        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);

            //if dimension is missing make the two operatants to have same size
            // TODO: use a for loop??
            size_t index = 0;
            {
                size_t rows = Inputs(index)->GetNumRows() == 0 ? Inputs(1 - index)->GetNumRows() : Inputs(index)->GetNumRows();
                size_t cols = Inputs(index)->GetNumCols() == 0 ? Inputs(1 - index)->GetNumCols() : Inputs(index)->GetNumCols();
                ValidateInferChildDims(index, rows, cols);
            }

            index = 1;
            {
                size_t rows = Inputs(index)->GetNumRows() == 0 ? Inputs(1 - index)->GetNumRows() : Inputs(index)->GetNumRows();
                size_t cols = Inputs(index)->GetNumCols() == 0 ? Inputs(1 - index)->GetNumCols() : Inputs(index)->GetNumCols();
                ValidateInferChildDims(index, rows, cols);
            }

            if (isFinalValidationPass && (Inputs(1)->GetNumRows() != Inputs(0)->GetNumRows() || Inputs(1)->GetNumCols() != Inputs(0)->GetNumCols()))
                LogicError("The Matrix dimension in the CosDistanceWithNegativeSamples operation does not match.");

            // input(2) is shift, input(3) is the #neg
            size_t negNumber = (size_t)Inputs(3)->FunctionValues()(0, 0);

            Resize(negNumber + 1, Inputs(1)->GetNumCols());

            InferMBLayoutFromInputsForStandardCase();
            InferImageDimsFromInputs();
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(0, false);

            m_outputImageLayout = ImageLayout();
        }

        virtual void MoveMatricesToDevice(const short deviceId)
        {
            Base::MoveMatricesToDevice(deviceId);
            m_invNorm0->TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_invNorm1->TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_invNormSquare->TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_leftTerm->TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_rightTerm->TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
            m_temp->TransferToDeviceIfNotThereAndNotAutoPlace(deviceId);
        }

        virtual void CopyTo(const ComputationNodePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = dynamic_pointer_cast<CosDistanceWithNegativeSamplesNode<ElemType>>(nodeP);
                *node->m_invNorm0 = *m_invNorm0;
                *node->m_invNorm1 = *m_invNorm1;
                *node->m_invNormSquare = *m_invNormSquare;
                *node->m_leftTerm = *m_leftTerm;
                *node->m_rightTerm = *m_rightTerm;
                *node->m_temp = *m_temp;
            }
        }
        //request matrices needed to do node function value evaluation
        virtual void RequestMatricesBeforeEval(MatrixPool& matrixPool)
        {
            Base::RequestMatricesBeforeEval(matrixPool);
            RequestMatrixFromPool(m_invNorm0, matrixPool);
            RequestMatrixFromPool(m_invNorm1, matrixPool);
            RequestMatrixFromPool(m_leftTerm, matrixPool);
            RequestMatrixFromPool(m_rightTerm, matrixPool);
        }

        //request matrices that are needed for gradient computation
        virtual void RequestMatricesBeforeGradientComp(MatrixPool& matrixPool)
        {
            Base::RequestMatricesBeforeGradientComp(matrixPool);
            RequestMatrixFromPool(m_invNormSquare, matrixPool);
            RequestMatrixFromPool(m_temp, matrixPool);
        }

        //release gradient and temp matrices that no longer needed after all the children's gradients are computed.
        virtual void ReleaseMatricesAfterGradientComp(MatrixPool& matrixPool)
        {
            Base::ReleaseMatricesAfterGradientComp(matrixPool);
            ReleaseMatrixToPool(m_invNorm0, matrixPool);
            ReleaseMatrixToPool(m_invNorm1, matrixPool);
            ReleaseMatrixToPool(m_leftTerm, matrixPool);
            ReleaseMatrixToPool(m_rightTerm, matrixPool);
            ReleaseMatrixToPool(m_invNormSquare, matrixPool);
            ReleaseMatrixToPool(m_temp, matrixPool);
        }
private:
        // invNorm nodes tranfer data between EvaluateThisNode and ComputeInputPartial
        shared_ptr<Matrix<ElemType>> m_invNorm0;
        shared_ptr<Matrix<ElemType>> m_invNorm1;
        shared_ptr<Matrix<ElemType>> m_leftTerm;
        shared_ptr<Matrix<ElemType>> m_rightTerm;
        // the rest are temporaries, values don't need to be maintained
        shared_ptr<Matrix<ElemType>> m_invNormSquare;
        shared_ptr<Matrix<ElemType>> m_temp;
    };

    template class CosDistanceWithNegativeSamplesNode<float>;
    template class CosDistanceWithNegativeSamplesNode<double>;

    // -----------------------------------------------------------------------
    // StrideTimesNode (left, right, stride/*0=row, 1=col*/)
    // TODO: why is 'stride' an Input and not just an initialization parameter?
    // -----------------------------------------------------------------------

    /**
    Has a stride in particular dimensions of left matrix when doing times operation. 
    Example 1: column stride s
    A in d x [s x T1] 
    B in T1 x s
    C = A x B  in d x s, and each element is computed as 
    c_{i,k} = \sum_j a_{i,j*s+k} b_{j,k}
    where s is the stride in column.

    Example 2:
    A in [s x T1] x d
    B in d x s
    C = A x B  in T1 x s, and each element is computed as
    c_{i,k} = \sum_j a_{i*s+k,j} b_{j,k}
    where s is the stride in rows.

    Notice that s is equal to k. 
    */
    template<class ElemType>
    class StrideTimesNode : public ComputationNode<ElemType>, public NumInputs<3>
    {
        typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;
        static const std::wstring TypeName() { return L"StrideTimes"; }

        size_t m_strideDim; // the dimension index on which stride works 
        size_t m_stride;    // the stride 
    private:
        void UpdateStride(const Matrix<ElemType>& input1) 
        {
            m_stride = input1.GetNumCols();
        }
    public:
        StrideTimesNode(DEVICEID_TYPE deviceId, const wstring & name) :
            Base(deviceId, name),
            m_stride(1)
        { }
        // BUGBUG: This node needs to serialize and CopyTo m_stride

        virtual void /*ComputationNode::*/ComputeInputPartial(const size_t inputIndex, const FrameRange & frameRange) override
        {
            if (frameRange.IsAllFrames()) { NOT_IMPLEMENTED; return; } // TODO: remove these one by one. And why is this not implemented?
            if (inputIndex > 2)
                InvalidArgument("StrideTimes operation only takes three inputs.");
            else if (inputIndex == 2)
                return;     // that's a constant

            Matrix<ElemType> sliceOutputGrad = GradientSlice(frameRange);

            if (m_strideDim == 1) // column stride
            {
                if (inputIndex == 0)  //left derivative
                {
                    Matrix<ElemType> sliceInput1Value = Inputs(1)->ValueSlice(frameRange);

                    //ComputeInputPartialLeft1(sliceInput1Value, Inputs(0)->GradientValues(), sliceOutputGrad);

                    size_t r = Inputs(0)->GetNumRows();
                    size_t T1 = Inputs(0)->GetNumCols() / GetNumParallelSequences();    // TODO: if T1 == GetNumTimeSteps() then we can simplify code below.
                    Matrix<ElemType> mTmp1(r, T1, sliceInput1Value.GetDeviceId());

                    // process sequence by sequence
                    for (size_t k = 0; k < GetNumParallelSequences(); k++)
                    {
                        mTmp1.SetValue(0);
                        auto mTmp2 = sliceInput1Value.ColumnSlice(k, 1);
                        auto mTmp3 = sliceOutputGrad.ColumnSlice(k, 1);

                        ComputeInputPartialLeft1(mTmp2, mTmp1, mTmp3);

                        for (size_t t = 0; t < T1; t++)
                        {
                            Inputs(0)->GradientValues().ColumnSlice(t*GetNumParallelSequences() + k, 1) += mTmp1.ColumnSlice(t, 1);
                        }
                    }
                }
                else  //right derivative
                {
                    Matrix<ElemType> sliceInput1Grad = Inputs(1)->GradientSlice(frameRange);

                    //ComputeInputPartialRight(Inputs(0)->FunctionValues(), sliceInput1Grad, sliceOutputGrad);

                    // process sequence by sequence
                    for (size_t k = 0; k < GetNumParallelSequences(); k++)
                    {
                        size_t r = Inputs(0)->GetNumRows();
                        size_t T1 = Inputs(0)->GetNumCols() / GetNumParallelSequences();    // TODO: if T1 == GetNumTimeSteps() then we can simplify code below.
                        Matrix<ElemType> mTmp1(r, T1, sliceOutputGrad.GetDeviceId());
                        for (size_t t = 0; t < T1; t++)
                        {
                            mTmp1.ColumnSlice(t, 1).SetValue(Inputs(0)->FunctionValues().ColumnSlice(t*GetNumParallelSequences() + k, 1));
                        }
                        auto mTmp2 = sliceInput1Grad.ColumnSlice(k, 1);
                        auto mTmp3 = sliceOutputGrad.ColumnSlice(k, 1);

                        ComputeInputPartialRight(mTmp1, mTmp2, mTmp3);
                    }
                }
            }
            else if (m_strideDim == 0) // row stride
            {
                if (inputIndex == 0)  //left derivative
                {
                    Matrix<ElemType> sliceInput1Value = Inputs(1)->ValueSlice(frameRange);

                    for (size_t k = 0; k < GetNumParallelSequences(); k++)
                    {
                        size_t d = Inputs(1)->GetNumRows();
                        size_t T1 = Inputs(0)->GetNumRows() / GetNumParallelSequences();
                        Matrix<ElemType> mTmp1(sliceInput1Value.GetDeviceId());
                        mTmp1.Resize(d, T1);
                        Matrix<ElemType> mTmp2 = sliceInput1Value.ColumnSlice(k, 1);
                        Matrix<ElemType> mTmp3 = sliceOutputGrad.ColumnSlice(k, 1);
                        ComputeInputPartialLeft(mTmp2, mTmp1, mTmp3);

                        Matrix<ElemType> mTmp4(sliceInput1Value.GetDeviceId());
                        for (size_t t = 0; t < T1; t++)
                        {
                            mTmp4 = mTmp1.ColumnSlice(t, 1);
                            mTmp4.Reshape(1, d);
                            Inputs(0)->GradientValues().AddToRowSliceValuesOf(mTmp4, t*GetNumParallelSequences() + k, 1);
                        }
                    }
                }
                else  //right derivative
                {
                    Matrix<ElemType> sliceInput1Grad = Inputs(1)->GradientSlice(frameRange);

                    for (size_t k = 0; k < GetNumParallelSequences(); k++)
                    {
                        size_t d = Inputs(1)->GetNumRows();
                        size_t T1 = Inputs(0)->GetNumRows() / GetNumParallelSequences();

                        Matrix<ElemType> mTmp0(sliceOutputGrad.GetDeviceId());
                        mTmp0.Resize(1, d);

                        Matrix<ElemType> mTmp1(sliceOutputGrad.GetDeviceId());
                        mTmp1.Resize(T1, d);
                        for (size_t t = 0; t < T1; t++)
                        {
                            mTmp0.SetValue(0);
                            mTmp0.AddWithRowSliceValuesOf(Inputs(0)->FunctionValues(), t * GetNumParallelSequences() + k, 1);
                            mTmp1.AssignToRowSliceValuesOf(mTmp0, t, 1);
                        }
                        Matrix<ElemType> mTmp2 = sliceInput1Grad.ColumnSlice(k, 1);
                        Matrix<ElemType> mTmp3 = sliceOutputGrad.ColumnSlice(k, 1);

                        ComputeInputPartialRight(mTmp1, mTmp2, mTmp3);
                    }
                }
            }
        }

        // TODO: the following two functions only differ in the order of argument use in the final MultiplyAndAdd()  --is that intended??
        static /*TODO: merge with call site*/void ComputeInputPartialLeft1(const Matrix<ElemType>& inputFunctionValues, Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)
        {
#if DUMPOUTPUT
            gradientValues.Print("Gradient-in");
            inputGradientValues.Print("child Gradient-in/out");
            inputFunctionValues.Print("child Function values");
#endif
            //currently we only support one combination when the input is sparse.
            if (inputFunctionValues.GetMatrixType() == SPARSE && inputGradientValues.GetMatrixType() == DENSE && gradientValues.GetMatrixType() == DENSE)
                inputGradientValues.SwitchToMatrixType(SPARSE, MatrixFormat::matrixFormatSparseBlockCol, false);

            Matrix<ElemType>::MultiplyAndAdd(gradientValues, false, inputFunctionValues, true, inputGradientValues);
#if DUMPOUTPUT
            inputGradientValues.Print("child Gradient-out");
#endif
        }

        static /*TODO: merge with call site*/void ComputeInputPartialLeft(Matrix<ElemType>& inputFunctionValues, Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)
        {
#if DUMPOUTPUT   
            gradientValues.Print("Gradient-in");   
            inputGradientValues.Print("child Gradient-in/out");   
            inputFunctionValues.Print("child Function values");   
#endif
            //currently we only support one combination when the input is sparse.   
            if (inputFunctionValues.GetMatrixType() == SPARSE && inputGradientValues.GetMatrixType() == DENSE && gradientValues.GetMatrixType() == DENSE)
                inputGradientValues.SwitchToMatrixType(SPARSE, MatrixFormat::matrixFormatSparseBlockCol, false);

            Matrix<ElemType>::MultiplyAndAdd(inputFunctionValues, false, gradientValues, true, inputGradientValues);

#if DUMPOUTPUT
            inputGradientValues.Print("child Gradient-out");
#endif
        }

        static /*TODO: merge with call site*/void ComputeInputPartialRight(Matrix<ElemType>& inputFunctionValues, Matrix<ElemType>& inputGradientValues, const Matrix<ElemType>& gradientValues)
        {
#if DUMPOUTPUT   
            gradientValues.Print("Gradient-in");   
            inputGradientValues.Print("child Gradient-in/out");
            inputFunctionValues.Print("child Function values");
#endif   
            Matrix<ElemType>::MultiplyAndAdd(inputFunctionValues, true, gradientValues, false, inputGradientValues);
#if DUMPOUTPUT
            inputGradientValues.Print("child Gradient-out");
#endif
        }

        virtual void /*ComputationNode::*/EvaluateThisNode(const FrameRange & frameRange) override
        {
            size_t rows0 = Inputs(0)->GetNumRows(), cols1 = Inputs(1)->GetNumCols();
            Matrix<ElemType> sliceInput1Value = Inputs(1)->ValueSlice(frameRange);
            UpdateStride(sliceInput1Value);

            if (m_strideDim == 0)
                Resize(rows0 / GetNumParallelSequences(), cols1);
            if (m_strideDim == 1)
                Resize(rows0, cols1);

            Matrix<ElemType> sliceOutputValue = ValueSlice(frameRange);

            // (TODO: these following assignments are leftovers of refactoring and can be short-circuited)
            Matrix<ElemType>& functionValues = sliceOutputValue;
            const Matrix<ElemType>& input0 = Inputs(0)->FunctionValues();
            const Matrix<ElemType>& input1 = sliceInput1Value;

            /**
            A in d x [s x T1]
            B in T1 x s
            C = A x B  in d x s, and each element is computed as 
            c_{i,k} = \sum_j a_{i,j*s+k} b_{j,k}
            C in d x s
            where s is the stride in column.
    
            Example 2:
            A in [s x T1] x d
            B in d x s
            C = A x B  in T1 x s, and each element is computed as
            c_{i,k} = \sum_j a_{i*s+k,j} b_{j,k}
            where s is the stride in rows.
            C in T1 x s
    
            strideDim : 0 or 1 (meaning to apply to row or column)
            */
#if DUMPOUTPUT
            input0.Print("StrideTimesNode - Input0");
#endif
            assert(m_strideDim == 0 || m_strideDim == 1);
            Matrix<ElemType> mTmp1(input0.GetDeviceId());
            Matrix<ElemType> mTmp2(input0.GetDeviceId());
            if (m_strideDim == 1) // 1 = col stride; the example 1 case at column
            {
                assert(m_stride == input1.GetNumCols());
                size_t T1 = input0.GetNumCols() / m_stride;
                assert(T1 == input1.GetNumRows());
                size_t d = input0.GetNumRows();
                functionValues.Resize(d, m_stride);
                for (size_t k = 0; k < m_stride; k++)
                {
                    mTmp1.Resize(d, T1);
                    for (size_t j = 0; j < T1; j++)
                    {
                        mTmp1.ColumnSlice(j, 1).SetValue(input0.ColumnSlice(j * m_stride + k, 1));
                    }

                    mTmp2 = input1.ColumnSlice(k, 1);
                    functionValues.ColumnSlice(k, 1).AssignProductOf(mTmp1, false, mTmp2, false);

                }
            }
            else if (m_strideDim == 0) // 0 = row stride; the example 2 case at row
            {
                assert(m_stride == input1.GetNumCols());
                size_t T1 = input0.GetNumRows() / m_stride;
                size_t d = input1.GetNumRows();
                assert(d == input0.GetNumCols());
                functionValues.Resize(T1, m_stride);
                mTmp1.Resize(d, T1);
                for (size_t k = 0; k < m_stride; k++)
                {
                    for (size_t j = 0; j < T1; j++)
                    {
                        mTmp1.ColumnSlice(j, 1).AssignRowSliceValuesOf(input0, k + j * m_stride, 1);
                    }

                    mTmp2 = input1.ColumnSlice(k, 1);
                    functionValues.ColumnSlice(k, 1).AssignProductOf(mTmp1, true, mTmp2, false);

                }
            }
#if NANCHECK
            functionValues.HasNan("StrideTimes");
#endif
#if DUMPOUTPUT
            functionValues.Print("StrideTimesNode");
#endif
        }

        /**
        three inputs
        input0: left matrix
        input1: right matrix
        stridedim: single element no gradient matrix, 0 row stride / 1 column stride
        */
        virtual void /*ComputationNodeBase::*/Validate(bool isFinalValidationPass) override
        {
            Base::Validate(isFinalValidationPass);

            if (Inputs(2)->FunctionValues().GetNumElements() != 1)
                RuntimeError("%ls %ls operation: Input(2) should be a single element matrix and have the value 0 (row) or 1 (col).", NodeName().c_str(), OperationName().c_str());
            m_strideDim = (size_t) Inputs(2)->FunctionValues().Get00Element();
            if (m_strideDim != 0 && m_strideDim != 1)
                RuntimeError("%ls %ls operation: Input(2) should be a single element matrix and have the value 0 (row) or 1 (col).", NodeName().c_str(), OperationName().c_str());
            //if (Inputs(2)->m_needGradient)        // disabled because this is a flag that belongs to Network. Node should simply not propagate anything into it
            //    RuntimeError("StrideTimes: No gradient update should be on input(2).");

            size_t rows0 = Inputs(0)->GetNumRows(), cols0 = Inputs(0)->GetNumCols();
            size_t rows1 = Inputs(1)->GetNumRows(), cols1 = Inputs(1)->GetNumCols();

            if (m_strideDim == 0) // by row
            {
                if (isFinalValidationPass && rows1 != cols0)
                    RuntimeError("The Matrix dimension in the StrideTimes operation in dim %d does not match for cols %d in A and rows %d in B.", m_strideDim, cols0, rows1);
                size_t T1 = rows0 / m_stride;
                Resize(T1, cols1);
            }

            else // by col
            {
                if (isFinalValidationPass && cols0 != rows1 * m_stride)
                    RuntimeError("The Matrix dimension in the StrideTimes operation in dim %d does not match for cols %d in A and row number %d in B.", m_strideDim, cols0, rows1);
                Resize(rows0, cols1);
            }
            LinkToMBLayout(Inputs(1)->GetMBLayout());   // retains the layout of the right input

            InferImageDimsFromInputs();
        }

        virtual void InferImageDimsFromInputs()
        {
            InferImageDimsFromInput(1, false); //the second one is the input since it's column wize

            //after multiplication the structure is lost
            m_outputImageLayout = ImageLayout(1, Inputs(0)->GetNumRows(), 1);
        }
    };

    template class StrideTimesNode<float>;
    template class StrideTimesNode<double>;

}}}
