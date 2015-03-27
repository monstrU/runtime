//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
//

/*XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XX                                                                           XX
XX                          AssertionProp                                    XX
XX                                                                           XX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
*/

#include "jitpch.h"
#ifdef _MSC_VER
#pragma hdrstop
#endif


/*****************************************************************************
 *
 *  Helper passed to Compiler::fgWalkTreePre() to find the Asgn node for optAddCopies()
 */

/* static */
Compiler::fgWalkResult  Compiler::optAddCopiesCallback(GenTreePtr * pTree, fgWalkData *data)
{
    GenTreePtr tree = *pTree;

    if (tree->OperKind() & GTK_ASGOP)
    {
        GenTreePtr op1  = tree->gtOp.gtOp1;
        Compiler * comp = data->compiler;

        if ((op1->gtOper == GT_LCL_VAR) &&
            (op1->gtLclVarCommon.gtLclNum == comp->optAddCopyLclNum))
        {
            comp->optAddCopyAsgnNode = tree;
            return WALK_ABORT;
        }
    }
    return WALK_CONTINUE;
}

                
/*****************************************************************************
 *
 *  Add new copies before Assertion Prop.
 */

void                Compiler::optAddCopies()
{
    unsigned        lclNum;
    LclVarDsc   *   varDsc;

#ifdef DEBUG
    if  (verbose) 
        printf("\n*************** In optAddCopies()\n\n");
    if (verboseTrees)
    {
        printf("Blocks/Trees at start of phase\n");
        fgDispBasicBlocks(true);
    }
#endif

    // Don't add any copies if we have reached the tracking limit.
    if  (lvaHaveManyLocals())
        return;

    for (lclNum = 0, varDsc = lvaTable;
         lclNum < lvaCount;
         lclNum++  , varDsc++)
    {
        var_types   typ = varDsc->TypeGet();

        // We only add copies for non temp local variables 
        // that have a single def and that can possibly be enregistered

        if (varDsc->lvIsTemp || !varDsc->lvSingleDef || !varTypeCanReg(typ))
            continue;

        /* For lvNormalizeOnLoad(), we need to add a cast to the copy-assignment
           like "copyLclNum = int(varDsc)" and optAssertionGen() only
           tracks simple assignments. The same goes for lvNormalizedOnStore as
           the cast is generated in fgMorphSmpOpAsg. This boils down to not having
           a copy until optAssertionGen handles this*/
        if (varDsc->lvNormalizeOnLoad() || varDsc->lvNormalizeOnStore() || typ == TYP_BOOL)
            continue;

        if (varTypeIsSmall(varDsc->TypeGet()) || typ == TYP_BOOL)
            continue;

        // If locals must be initialized to zero, that initialization counts as a second definition.
        // VB in particular allows usage of variables not explicitly initialized.
        // Note that this effectively disables this optimization for all local variables
        // as C# sets InitLocals all the time starting in Whidbey.

        if (!varDsc->lvIsParam && info.compInitMem)
            continue;

#ifdef DEBUG
        if (lvaTable[lclNum].lvDblWasInt)
            continue;
#endif

        // On x86 we may want to add a copy for an incoming double parameter
        // because we can ensure that the copy we make is double aligned
        // where as we can never ensure the alignment of an incoming double parameter
        // 
        // On all other platforms we will never need to make a copy 
        // for an incoming double parameter

        bool isFloatParam = false;

#ifdef _TARGET_X86_
        isFloatParam = varDsc->lvIsParam && varTypeIsFloating(typ);
#endif

        if (!isFloatParam && !varDsc->lvVolatileHint)
            continue;

        // We don't want to add a copy for a variable that is part of a struct
        if (varDsc->lvIsStructField)
            continue;

        // We require that the weighted ref count be significant.
        if (varDsc->lvRefCntWtd <= (BB_LOOP_WEIGHT*BB_UNITY_WEIGHT/2))
            continue;

        // For parameters, we only want to add a copy for the heavier-than-average
        // uses instead of adding a copy to cover every single use.
        // 'paramImportantUseDom' is the set of blocks that dominate the
        // heavier-than-average uses of a parameter.        
        // Initial value is all blocks.

        BlockSet BLOCKSET_INIT_NOCOPY(paramImportantUseDom, BlockSetOps::MakeFull(this));

        // This will be threshold for determining heavier-than-average uses
        unsigned paramAvgWtdRefDiv2 = (varDsc->lvRefCntWtd + varDsc->lvRefCnt/2) / (varDsc->lvRefCnt*2);

        bool paramFoundImportantUse = false;

#ifdef DEBUG
        if  (verbose) 
        {
            printf("Trying to add a copy for V%02u %s, avg_wtd = %s\n", 
                   lclNum, 
                   varDsc->lvIsParam ? "an arg"
                                     : "a local", 
                   refCntWtd2str(paramAvgWtdRefDiv2));
        }
#endif

        //
        // We must have a ref in a block that is dominated only by the entry block
        //

        if (BlockSetOps::MayBeUninit(varDsc->lvRefBlks))
        {
            // No references
            continue;
        }

        bool isDominatedByFirstBB = false;

        BLOCKSET_ITER_INIT(this, iter, varDsc->lvRefBlks, blkNum);
        while (iter.NextElem(this, &blkNum))
        {
            /* Find the block 'blkNum' */
            BasicBlock * block = fgFirstBB;
            while (block && (block->bbNum != blkNum))
            {
                block = block->bbNext;
            }
            noway_assert(block && (block->bbNum == blkNum));

            bool        importantUseInBlock = (varDsc->lvIsParam) && (block->getBBWeight(this) > paramAvgWtdRefDiv2);
            bool        isPreHeaderBlock    = ((block->bbFlags & BBF_LOOP_PREHEADER) != 0);
            BlockSet    BLOCKSET_INIT_NOCOPY(blockDom, BlockSetOps::UninitVal());
            BlockSet    BLOCKSET_INIT_NOCOPY(blockDomSub0, BlockSetOps::UninitVal());

            if (block->bbIDom == nullptr && isPreHeaderBlock)
            {
                // Loop Preheader blocks that we insert will have a bbDom set that is nullptr
                // but we can instead use the bNext successor block's dominator information
                noway_assert(block->bbNext != nullptr);
                BlockSetOps::AssignNoCopy(this, blockDom, fgGetDominatorSet(block->bbNext));
            }
            else
            {
                BlockSetOps::AssignNoCopy(this, blockDom, fgGetDominatorSet(block));
            }

            if (!BlockSetOps::IsEmpty(this, blockDom))
            {
                BlockSetOps::Assign(this, blockDomSub0, blockDom);
                if (isPreHeaderBlock)
                {
                    // We must clear bbNext block number from the dominator set
                    BlockSetOps::RemoveElemD(this, blockDomSub0, block->bbNext->bbNum);
                }
                /* Is this block dominated by fgFirstBB? */
                if (BlockSetOps::IsMember(this, blockDomSub0, fgFirstBB->bbNum))
                {
                    isDominatedByFirstBB = true;
                }
            }

#ifdef DEBUG
            if  (verbose)
            {
                printf("        Referenced in BB%02u, bbWeight is %s", 
                                blkNum,
                                refCntWtd2str(block->getBBWeight(this)));

                if (isDominatedByFirstBB)
                    printf(", which is dominated by BB01");

                if (importantUseInBlock)
                    printf(", ImportantUse");

                printf("\n");
            }
#endif

            /* If this is a heavier-than-average block, then track which
               blocks dominate this use of the parameter. */
            if (importantUseInBlock)
            {
                paramFoundImportantUse = true;
                BlockSetOps::IntersectionD(this, paramImportantUseDom, blockDomSub0); // Clear blocks that do not dominate
            }
        }

        // We should have found at least one heavier-than-averageDiv2 block.
        if (varDsc->lvIsParam)
        {
            if (!paramFoundImportantUse)
                continue;
        }

        // For us to add a new copy:
        // we require that we have a floating point parameter
        // or a lvVolatile variable that is always reached from the first BB
        // and we have at least one block available in paramImportantUseDom
        //
        bool doCopy = (isFloatParam || (isDominatedByFirstBB && varDsc->lvVolatileHint))
                        && !BlockSetOps::IsEmpty(this, paramImportantUseDom);
            
        // Under stress mode we expand the number of candidates
        // to include parameters of any type 
        // or any variable that is always reached from the first BB
        //
        if (compStressCompile(STRESS_GENERIC_VARN, 30))
        {
            // Ensure that we preserve the invariants required by the subsequent code.
            if (varDsc->lvIsParam || isDominatedByFirstBB)
                doCopy = true;
        }

        if (!doCopy)
            continue;

        GenTreePtr stmt;
        unsigned copyLclNum = lvaGrabTemp(false DEBUGARG("optAddCopies"));

        // Because lvaGrabTemp may have reallocated the lvaTable, ensure varDsc
        // is still in sync with lvaTable[lclNum];
        varDsc = &lvaTable[lclNum];
    
        // Set lvType on the new Temp Lcl Var
        lvaTable[copyLclNum].lvType = typ;

#ifdef DEBUG
        if  (verbose) 
        {
            printf("\n    Finding the best place to insert the assignment V%02i=V%02i\n", 
                   copyLclNum, lclNum);
        }
#endif

        if (varDsc->lvIsParam)
        {
            noway_assert(varDsc->lvDefStmt == nullptr || varDsc->lvIsStructField);

            // Create a new copy assignment tree
            GenTreePtr copyAsgn = gtNewTempAssign(copyLclNum, gtNewLclvNode(lclNum, typ));

            /* Find the best block to insert the new assignment     */
            /* We will choose the lowest weighted block, and within */
            /* those block, the highest numbered block which        */
            /* dominates all the uses of the local variable         */

            /* Our default is to use the first block */
            BasicBlock * bestBlock  = fgFirstBB;
            unsigned     bestWeight = bestBlock->getBBWeight(this);
            BasicBlock * block      = bestBlock;

#ifdef DEBUG
            if  (verbose)
            {
                printf("        Starting at BB%02u, bbWeight is %s",
                       block->bbNum, 
                       refCntWtd2str(block->getBBWeight(this)));

                printf(", bestWeight is %s\n", 
                       refCntWtd2str(bestWeight));
            }
#endif

            /* We have already calculated paramImportantUseDom above. */

            BLOCKSET_ITER_INIT(this, iter, paramImportantUseDom, blkNum);
            while (iter.NextElem(this, &blkNum))
            {
                /* Advance block to point to 'blkNum' */
                /* This assumes that the iterator returns block number is increasing lexical order. */
                while (block && (block->bbNum != blkNum))
                {
                    block = block->bbNext;
                }
                noway_assert(block && (block->bbNum == blkNum));

#ifdef DEBUG
                if  (verbose)
                {
                    printf("        Considering BB%02u, bbWeight is %s",
                           block->bbNum, 
                           refCntWtd2str(block->getBBWeight(this)));

                    printf(", bestWeight is %s\n",
                           refCntWtd2str(bestWeight));
                }
#endif

                // Does this block have a smaller bbWeight value? 
                if (block->getBBWeight(this) > bestWeight)
                {
#ifdef DEBUG
                    if  (verbose)
                        printf("bbWeight too high\n");
#endif
                    continue;
                }

                // Don't use blocks that are exception handlers because
                // inserting a new first statement will interface with 
                // the CATCHARG

                if (handlerGetsXcptnObj(block->bbCatchTyp))
                {
#ifdef DEBUG
                    if  (verbose)
                        printf("Catch block\n");
#endif
                    continue;
                }

                // Don't use the BBJ_ALWAYS block marked with BBF_KEEP_BBJ_ALWAYS. These
                // are used by EH code. The JIT can not generate code for such a block.

                if (block->bbFlags & BBF_KEEP_BBJ_ALWAYS)
                {
#if FEATURE_EH_FUNCLETS
                    // With funclets, this is only used for BBJ_CALLFINALLY/BBJ_ALWAYS pairs. For x86, it is also used as the "final step" block for leaving finallys.
                    assert((block->bbPrev != nullptr) && block->bbPrev->isBBCallAlwaysPair());
#endif // FEATURE_EH_FUNCLETS
#ifdef DEBUG
                    if  (verbose)
                        printf("Internal EH BBJ_ALWAYS block\n");
#endif
                    continue;
                }

                // This block will be the new candidate for the insert point
                // for the new assignment
                //
#ifdef DEBUG
                if  (verbose)
                    printf("new bestBlock\n");
#endif

                bestBlock  = block;
                bestWeight = block->getBBWeight(this);
            }

            /* If there is a use of the variable in this block */
            /* then we insert the assignment at the beginning  */
            /* otherwise we insert the statement at the end    */

#ifdef DEBUG
            if  (verbose)
            {
                printf("        Insert copy at the %s of BB%02u\n",
                       (BlockSetOps::IsEmpty(this, paramImportantUseDom) || BlockSetOps::IsMember(this, varDsc->lvRefBlks, bestBlock->bbNum)) ?
                       "start" : "end",
                       bestBlock->bbNum); 
            }
#endif

            if (BlockSetOps::IsEmpty(this, paramImportantUseDom) || BlockSetOps::IsMember(this, varDsc->lvRefBlks, bestBlock->bbNum))
                stmt = fgInsertStmtAtBeg(bestBlock, copyAsgn);
            else
                stmt = fgInsertStmtNearEnd(bestBlock, copyAsgn);

            /* Increment its lvRefCnt and lvRefCntWtd */
            lvaTable[lclNum].incRefCnts(fgFirstBB->getBBWeight(this), this);

            /* Increment its lvRefCnt and lvRefCntWtd */
            lvaTable[copyLclNum].incRefCnts(fgFirstBB->getBBWeight(this), this);
        }
        else
        {
            noway_assert(varDsc->lvDefStmt != nullptr);

            /* Locate the assignment to varDsc in the lvDefStmt */
            stmt = varDsc->lvDefStmt;
            noway_assert(stmt->gtOper == GT_STMT);

            optAddCopyLclNum   = lclNum; // in
            optAddCopyAsgnNode = nullptr;   // out
        
            fgWalkTreePre(&stmt->gtStmt.gtStmtExpr, 
                          Compiler::optAddCopiesCallback, 
                          (void *) this, 
                          false);

            noway_assert(optAddCopyAsgnNode);

            GenTreePtr tree = optAddCopyAsgnNode;
            GenTreePtr op1  = tree->gtOp.gtOp1;

            noway_assert( tree && op1                   &&
                   (tree->OperKind() & GTK_ASGOP) &&
                   (op1->gtOper == GT_LCL_VAR)    &&
                   (op1->gtLclVarCommon.gtLclNum == lclNum));

            /*  TODO-Review: BB_UNITY_WEIGHT is not the correct block weight */
            unsigned   blockWeight = BB_UNITY_WEIGHT;

            /* Increment its lvRefCnt and lvRefCntWtd twice */
            lvaTable[copyLclNum].incRefCnts(blockWeight, this);
            lvaTable[copyLclNum].incRefCnts(blockWeight, this);

            /* Assign the old expression into the new temp */

            GenTreePtr newAsgn  = gtNewTempAssign(copyLclNum, tree->gtOp.gtOp2);

            /* Copy the new temp to op1 */

            GenTreePtr copyAsgn = gtNewAssignNode(op1, gtNewLclvNode(copyLclNum, typ));

            /* Change the tree to a GT_COMMA with the two assignments as child nodes */

            tree->gtBashToNOP();
            tree->ChangeOper(GT_COMMA);

            tree->gtOp.gtOp1  = newAsgn;
            tree->gtOp.gtOp2  = copyAsgn;

            tree->gtFlags    |= ( newAsgn->gtFlags & GTF_GLOB_EFFECT);
            tree->gtFlags    |= (copyAsgn->gtFlags & GTF_GLOB_EFFECT);
        }

#ifdef DEBUG
        if  (verbose)
        {
            printf("\nIntroducing a new copy for V%02u\n", lclNum);
            gtDispTree(stmt->gtStmt.gtStmtExpr);
            printf("\n");
        }
#endif
    }
}


/*****************************************************************************
 *
 *  Initialize the assertion prop tracking logic.
 */

void                Compiler::optAssertionInit(bool isLocalProp)
{
    unsigned        lclNum;
    LclVarDsc   *   varDsc;

    for (lclNum = 0, varDsc = lvaTable;
         lclNum < lvaCount;
         lclNum++  , varDsc++)
    {
        varDsc->lvAssertionDep = 0;
    }

    optAssertionCount    = 0;
    optAssertionPropagated = false;
    optLocalAssertionProp  = isLocalProp;
    bbJtrueAssertionOut = nullptr;

    if (!isLocalProp)
    {
        optValueNumToAsserts = new (getAllocator()) ValueNumToAssertsMap(getAllocator());
    }
}

#ifdef DEBUG
void Compiler::optPrintAssertion(AssertionDsc*  curAssertion, unsigned assertionIndex /* =0 */)
{
    if (curAssertion->op1.kind == O1K_EXACT_TYPE)
    {
        printf("Type     ");
    }
    else if (curAssertion->op1.kind == O1K_ARR_BND)
    {
        printf("ArrBnds  ");
    }
    else if (curAssertion->op1.kind == O1K_SUBTYPE)
    {
        printf("Subtype  ");
    }
    else if (curAssertion->op2.kind == O2K_LCLVAR_COPY)
    {
        printf("Copy     ");
    }
    else if  ((curAssertion->op2.kind == O2K_CONST_INT) ||
              (curAssertion->op2.kind == O2K_CONST_LONG) ||
              (curAssertion->op2.kind == O2K_CONST_DOUBLE)   )
    {
        printf("Constant ");
    }
    else if  (curAssertion->op2.kind == O2K_SUBRANGE)
    {
        printf("Subrange ");
    }
    else
    {
        printf("?assertion classification? ");
    }
    printf("Assertion: ");

    if (!optLocalAssertionProp)
    {
        printf("(" STR_VN "%x," STR_VN "%x) ", curAssertion->op1.vn, curAssertion->op2.vn);
    }

    if ((curAssertion->op1.kind == O1K_LCLVAR)      ||
        (curAssertion->op1.kind == O1K_EXACT_TYPE)  ||
        (curAssertion->op1.kind == O1K_SUBTYPE)       )
    {
        printf("V%02u", curAssertion->op1.lcl.lclNum);
        if (curAssertion->op1.lcl.ssaNum != SsaConfig::RESERVED_SSA_NUM)
        {
            printf(".%02u", curAssertion->op1.lcl.ssaNum);
        }
    }
    else if (curAssertion->op1.kind == O1K_ARR_BND)
    {
        printf("[idx:");
        vnStore->vnDump(this, curAssertion->op1.bnd.vnIdx);
        printf(";len:");
        vnStore->vnDump(this, curAssertion->op1.bnd.vnLen);
        printf("]");
    }
    else if (curAssertion->op1.kind == O1K_ARRLEN_OPER_BND)
    {
        printf("Oper_Bnd");
        vnStore->vnDump(this, curAssertion->op1.vn);
    }
    else if (curAssertion->op1.kind == O1K_ARRLEN_LOOP_BND)
    {
        printf("Loop_Bnd");
        vnStore->vnDump(this, curAssertion->op1.vn);
    }
    else
    {
        printf("?op1.kind?");
    }

    if (curAssertion->assertionKind == OAK_SUBRANGE)
    {
        printf(" in ");
    }
    else if (curAssertion->assertionKind == OAK_EQUAL)
    {
        if (curAssertion->op1.kind == O1K_LCLVAR)
            printf(" == ");
        else
            printf(" is ");
    }
    else if (curAssertion->assertionKind == OAK_NO_THROW)
    {
        printf(" in range ");
    }
    else if (curAssertion->assertionKind == OAK_NOT_EQUAL)
    {
        if (curAssertion->op1.kind == O1K_LCLVAR)
            printf(" != ");
        else
            printf(" is not ");
    }
    else
    {
        printf(" ?assertionKind? ");
    }

    if (curAssertion->op1.kind != O1K_ARR_BND)
    {
        switch (curAssertion->op2.kind)
        {
        case O2K_LCLVAR_COPY:
            printf("V%02u", curAssertion->op2.lcl.lclNum);
            if (curAssertion->op1.lcl.ssaNum != SsaConfig::RESERVED_SSA_NUM)
            {
                printf(".%02u", curAssertion->op1.lcl.ssaNum);
            }
            break;
    
        case O2K_CONST_INT:
        case O2K_IND_CNS_INT:
            if (curAssertion->op1.kind == O1K_EXACT_TYPE) 
            {
                printf("Exact Type MT(%08X)", dspPtr(curAssertion->op2.u1.iconVal));
                assert(curAssertion->op2.u1.iconFlags != 0);
            }
            else if (curAssertion->op1.kind == O1K_SUBTYPE) 
            {
                printf("MT(%08X)", dspPtr(curAssertion->op2.u1.iconVal));
                assert(curAssertion->op2.u1.iconFlags != 0);
            }
            else if (curAssertion->op1.kind == O1K_ARRLEN_OPER_BND)
            {
                assert(!optLocalAssertionProp);
                vnStore->vnDump(this, curAssertion->op2.vn);
            }
            else if (curAssertion->op1.kind == O1K_ARRLEN_LOOP_BND)
            {
                assert(!optLocalAssertionProp);
                vnStore->vnDump(this, curAssertion->op2.vn);
            }
            else
            {
                unsigned     lclNum = curAssertion->op1.lcl.lclNum; assert(lclNum < lvaCount);
                LclVarDsc *  varDsc = lvaTable + lclNum;
                if (varDsc->lvType == TYP_REF)
                {
                    assert(curAssertion->op2.u1.iconVal == 0);
                    printf("null");
                }
                else
                {
                    if ((curAssertion->op2.u1.iconFlags & GTF_ICON_HDL_MASK) != 0)
                        printf("[%08p]", dspPtr(curAssertion->op2.u1.iconVal));
                    else
                        printf("%d", curAssertion->op2.u1.iconVal);
                }
            }
            break;
    
        case O2K_CONST_LONG:
            printf("0x%016llx", curAssertion->op2.lconVal);
            break;
    
        case O2K_CONST_DOUBLE:
            if (*((__int64 *)&curAssertion->op2.dconVal) == (__int64)I64(0x8000000000000000))
                printf("-0.00000");
            else
                printf("%#lg", curAssertion->op2.dconVal); 
            break;
    
        case O2K_SUBRANGE:
            printf("[%d..%d]", curAssertion->op2.u2.loBound, curAssertion->op2.u2.hiBound);
            break;
    
        default:
            printf("?op2.kind?");
            break;
        }
    }

    if (assertionIndex > 0)
    {
        printf(" :: index=#%02u, mask=%s", assertionIndex, genES2str(genCSEnum2bit(assertionIndex)));
    }
    printf("\n");
}
#endif // DEBUG

/******************************************************************************
 *
 * Helper to retrieve the "assertIndex" assertion. Note that assertIndex 0
 * is NO_ASSERTION_INDEX and "optAssertionCount" is the last valid index.
 *
 */
Compiler::AssertionDsc *  Compiler::optGetAssertion(unsigned assertIndex)
{
    assert(NO_ASSERTION_INDEX == 0);
    noway_assert(assertIndex != NO_ASSERTION_INDEX);

    if (assertIndex > MAX_ASSERTION_CNT)
    {
        return nullptr;
    }
    return &optAssertionTabPrivate[assertIndex - 1];
}

/*****************************************************************************
 *
 * A simple helper routine so not all callers need to supply a AssertionDsc*
 * if they don't care about it. Refer overloaded method optCreateAssertion.
 *
 */
unsigned Compiler::optCreateAssertion(GenTreePtr op1, GenTreePtr op2, optAssertionKind assertionKind)
{
    AssertionDsc assertionDsc;
    return optCreateAssertion(op1, op2, assertionKind, &assertionDsc);
}

/*****************************************************************************
 *
 *  We attempt to create the following assertion:
 *
 *     op1 assertionKind op2
 *
 *  If we can create the assertion then update 'assertion' if we are
 *  unsuccessful assertion->assertionKind will be OAK_INVALID. If we are
 *  successful in creating the assertion we call optAddAssertion which adds
 *  the assertion to our assertion table.
 * 
 *  If we are able to create the assertion the return value is the
 *  assertionIndex for this assertion otherwise the return value is
 *  NO_ASSERTION_INDEX and we could not create the assertion.
 *
 */
unsigned Compiler::optCreateAssertion(GenTreePtr op1, GenTreePtr op2,  
                                      optAssertionKind assertionKind, 
                                      AssertionDsc* assertion)
{
    memset(assertion, 0, sizeof(AssertionDsc));
    //
    // If we cannot create an assertion using op1 and op2 then the assertionKind
    // must be OAK_INVALID, so we initialize it to OAK_INVALID and only change it
    // to a valid assertion when everything is good.
    // 
    assertion->assertionKind = OAK_INVALID;
    bool haveArgs = false;
    var_types toType;

    if (op1->gtOper == GT_ARR_BOUNDS_CHECK)
    {
        if (assertionKind == OAK_NO_THROW)
        {
            GenTreeBoundsChk* arrBndsChk = op1->AsBoundsChk();
            assertion->assertionKind = assertionKind;
            assertion->op1.kind = O1K_ARR_BND;
            assertion->op1.bnd.vnIdx = arrBndsChk->gtIndex->gtVNPair.GetConservative();
            assertion->op1.bnd.vnLen = arrBndsChk->gtArrLen->gtVNPair.GetConservative();
            goto DONE_ASSERTION;
        }
    }

    //
    // Did we recieve Helper call args?
    //
    if (op1->gtOper == GT_LIST)
    {
        if (op2->gtOper != GT_LIST)
        {
            goto DONE_ASSERTION;  // Don't make an assertion
        }
        op1 = op1->gtOp.gtOp1;
        op2 = op2->gtOp.gtOp1;
        haveArgs = true;
    }

    //
    // Are we trying to make a non-null assertion?
    // 
    if (op2 == nullptr)
    {
        assert(haveArgs == false);
        //
        // Must an OAK_NOT_EQUAL assertion
        // 
        noway_assert(assertionKind == OAK_NOT_EQUAL);

        //
        // Set op1 to the instance pointer of the indirection
        // 
        if ((op1->gtOper == GT_ADD) && (op1->gtType == TYP_BYREF))
        {
            op1 = op1->gtOp.gtOp1;

            if ((op1->gtOper == GT_ADD) && (op1->gtType == TYP_BYREF))
            {
                op1 = op1->gtOp.gtOp1;
            }
        }

        if (op1->gtOper != GT_LCL_VAR)
        {
            goto DONE_ASSERTION;  // Don't make an assertion
        }

        unsigned lclNum = op1->gtLclVarCommon.gtLclNum;    noway_assert(lclNum  < lvaCount);
        LclVarDsc * lclVar = &lvaTable[lclNum];

        //
        // We only perform null-checks on GC refs 
        // so only make non-null assertions about GC refs
        // 
        if (lclVar->TypeGet() != TYP_REF)
        {
            goto DONE_ASSERTION;  // Don't make an assertion
        }

        //  If the local variable has its address exposed then bail 
        if (lclVar->lvAddrExposed)
        {
            goto DONE_ASSERTION;  // Don't make an assertion
        }

        assertion->op1.kind         = O1K_LCLVAR;
        assertion->op1.lcl.lclNum   = lclNum;
        assertion->op1.vn           = op1->gtVNPair.GetConservative();       
        assertion->op1.lcl.ssaNum   = op1->AsLclVarCommon()->GetSsaNum();
        assertion->assertionKind    = assertionKind;
        assertion->op2.kind         = O2K_CONST_INT;
        assertion->op2.vn           = ValueNumStore::VNForNull();
        assertion->op2.u1.iconVal   = 0;
        assertion->op2.u1.iconFlags = 0;
#ifdef _TARGET_64BIT_
        assertion->op2.u1.iconFlags |= 1; // Signify that this is really TYP_LONG
#endif // _TARGET_64BIT_
    }
    //
    // Are we making an assertion about a local variable?
    // 
    else if (op1->gtOper == GT_LCL_VAR)
    {
        unsigned lclNum = op1->gtLclVarCommon.gtLclNum;    noway_assert(lclNum  < lvaCount);
        LclVarDsc * lclVar = &lvaTable[lclNum];

        //  If the local variable has its address exposed then bail 
        if (lclVar->lvAddrExposed)
        {
            goto DONE_ASSERTION;  // Don't make an assertion
        }

#ifdef DEBUG
        if (lclVar->lvDblWasInt)
        {
            goto DONE_ASSERTION;  // Don't make an assertion
        }
#endif

        if (haveArgs)
        {
            //
            // Must either be an OAK_EQUAL or an OAK_NOT_EQUAL assertion
            // 
            if ((assertionKind != OAK_EQUAL) && (assertionKind != OAK_NOT_EQUAL))
            {
                goto DONE_ASSERTION;  // Don't make an assertion
            }

            if (op2->gtOper == GT_IND)
            {
                op2 = op2->gtOp.gtOp1;
                assertion->op2.kind = O2K_IND_CNS_INT;
            }
            else
            {
                assertion->op2.kind = O2K_CONST_INT;
            }

            if (op2->gtOper != GT_CNS_INT)
            {
                goto DONE_ASSERTION;  // Don't make an assertion
            }

            //
            // TODO-CQ: Check for Sealed class and change kind to O1K_EXACT_TYPE
            //          And consider the special cases, like CORINFO_FLG_SHAREDINST or CORINFO_FLG_VARIANCE
            //          where a class can be sealed, but they don't behave as exact types because casts to 
            //          non-base types sometimes still succeed.  
            //
            assertion->op1.kind         = O1K_SUBTYPE;
            assertion->op1.lcl.lclNum   = lclNum;
            assertion->op1.vn           = op1->gtVNPair.GetConservative();
            assertion->op1.lcl.ssaNum   = op1->AsLclVarCommon()->GetSsaNum();
            assertion->op2.u1.iconVal   = op2->gtIntCon.gtIconVal;
            assertion->op2.vn           = op2->gtVNPair.GetConservative();
            assertion->op2.u1.iconFlags = op2->GetIconHandleFlag();

            //
            // Ok everything has been set and the assertion looks good
            // 
            assertion->assertionKind = assertionKind;
        }
        else // !haveArgs
        {
            /* Skip over a GT_COMMA node(s), if necessary */
            while (op2->gtOper == GT_COMMA)
            {
                op2 = op2->gtOp.gtOp2;
            }

            assertion->op1.kind = O1K_LCLVAR;
            assertion->op1.lcl.lclNum = lclNum;
            assertion->op1.vn = op1->gtVNPair.GetConservative();
            assertion->op1.lcl.ssaNum = op1->AsLclVarCommon()->GetSsaNum();

            switch (op2->gtOper)
            {
            optOp2Kind op2Kind;
            //
            //  No Assertion
            //  
            default:
                goto DONE_ASSERTION;  // Don't make an assertion

            //
            //  Constant Assertions
            //  
            case GT_CNS_INT:
                op2Kind = O2K_CONST_INT;
                goto CNS_COMMON;

            case GT_CNS_LNG:
                op2Kind = O2K_CONST_LONG;
                goto CNS_COMMON;

            case GT_CNS_DBL:
                op2Kind = O2K_CONST_DOUBLE;
                goto CNS_COMMON;

CNS_COMMON:
                {
                    //
                    // Must either be an OAK_EQUAL or an OAK_NOT_EQUAL assertion
                    // 
                    if ((assertionKind != OAK_EQUAL) && (assertionKind != OAK_NOT_EQUAL))
                    {
                        goto DONE_ASSERTION;  // Don't make an assertion
                    }

                    // If the LclVar is a TYP_LONG then we only make 
                    // assertions where op2 is also TYP_LONG
                    // 
                    if ((lclVar->TypeGet() == TYP_LONG) && (op2->TypeGet() != TYP_LONG))
                    {
                        goto DONE_ASSERTION;  // Don't make an assertion
                    }

                    assertion->op2.kind = op2Kind;
                    assertion->op2.lconVal = 0;
                    assertion->op2.vn = op2->gtVNPair.GetConservative();

                    if (op2->gtOper == GT_CNS_INT)
                    {
#ifdef _TARGET_ARM_
                        // Do not Constant-Prop large constants for ARM
                        if (!codeGen->validImmForMov(op2->gtIntCon.gtIconVal))
                        {
                            goto DONE_ASSERTION;  // Don't make an assertion
                        }
#endif // _TARGET_ARM_
                        assertion->op2.u1.iconVal   = op2->gtIntCon.gtIconVal;
                        assertion->op2.u1.iconFlags = op2->GetIconHandleFlag();
#ifdef _TARGET_64BIT_
                        if (op2->TypeGet() == TYP_LONG || op2->TypeGet() == TYP_BYREF)
                        {
                            assertion->op2.u1.iconFlags |= 1; // Signify that this is really TYP_LONG
                        }
#endif // _TARGET_64BIT_
                    }
                    else if (op2->gtOper == GT_CNS_LNG)
                    {
                        assertion->op2.lconVal = op2->gtLngCon.gtLconVal;
                    }
                    else
                    {
                        noway_assert(op2->gtOper == GT_CNS_DBL);
                        /* If we have an NaN value then don't record it */
                        if  (_isnan(op2->gtDblCon.gtDconVal))
                        {
                            goto DONE_ASSERTION;  // Don't make an assertion
                        }
                        assertion->op2.dconVal = op2->gtDblCon.gtDconVal;
                    }

                    //
                    // Ok everything has been set and the assertion looks good
                    // 
                    assertion->assertionKind = assertionKind;
                }
                break;

            //
            //  Copy Assertions
            //  
            case GT_LCL_VAR:
                {
                    //
                    // Must either be an OAK_EQUAL or an OAK_NOT_EQUAL assertion
                    // 
                    if ((assertionKind != OAK_EQUAL) && (assertionKind != OAK_NOT_EQUAL))
                    {
                        goto DONE_ASSERTION;  // Don't make an assertion
                    }

                    unsigned lclNum2 = op2->gtLclVarCommon.gtLclNum;    noway_assert(lclNum2  < lvaCount);
                    LclVarDsc * lclVar2 = &lvaTable[lclNum2];

                    // If the two locals are the same then bail
                    if (lclNum == lclNum2)
                    {
                        goto DONE_ASSERTION;  // Don't make an assertion
                    }
                
                    // If the types are different then bail */
                    if (lclVar->lvType != lclVar2->lvType)
                    {
                        goto DONE_ASSERTION;  // Don't make an assertion
                    }

                    //  If the local variable has its address exposed then bail 
                    if (lclVar2->lvAddrExposed)
                    {
                        goto DONE_ASSERTION;  // Don't make an assertion
                    }

                    assertion->op2.kind = O2K_LCLVAR_COPY;
                    assertion->op2.lcl.lclNum = lclNum2;
                    assertion->op2.vn = op2->gtVNPair.GetConservative();
                    assertion->op2.lcl.ssaNum = op2->AsLclVarCommon()->GetSsaNum();
                    
                    //
                    // Ok everything has been set and the assertion looks good
                    // 
                    assertion->assertionKind = assertionKind;
                }
                break;

            //  Subrange Assertions
            case GT_EQ:
            case GT_NE:
            case GT_LT:
            case GT_LE:
            case GT_GT:
            case GT_GE:

                /* Assigning the result of a RELOP, we can add a boolean subrange assertion */

                toType = TYP_BOOL;
                goto SUBRANGE_COMMON;

            case GT_CLS_VAR:

                /* Assigning the result of an indirection into a LCL_VAR, see if we can add a subrange assertion */

                toType = op2->gtType;
                goto SUBRANGE_COMMON;

            case GT_ARR_ELEM:

                /* Assigning the result of an indirection into a LCL_VAR, see if we can add a subrange assertion */

                toType = op2->gtType;
                goto SUBRANGE_COMMON;

            case GT_LCL_FLD:

                /* Assigning the result of an indirection into a LCL_VAR, see if we can add a subrange assertion */

                toType = op2->gtType;
                goto SUBRANGE_COMMON;

            case GT_IND:

                /* Assigning the result of an indirection into a LCL_VAR, see if we can add a subrange assertion */

                toType = op2->gtType;
                goto SUBRANGE_COMMON;

            case GT_CAST:
                {
                    if (lvaTable[lclNum].lvIsStructField && lvaTable[lclNum].lvNormalizeOnLoad())
                    {   
                        // Keep the cast on small struct fields.
                        goto DONE_ASSERTION;  // Don't make an assertion
                    }

                    toType = op2->CastToType();
SUBRANGE_COMMON:
                    if ((assertionKind != OAK_SUBRANGE) && (assertionKind != OAK_EQUAL))
                    {
                        goto DONE_ASSERTION;  // Don't make an assertion
                    }

                    if (varTypeIsFloating(op1->TypeGet()))
                    {
                        // We don't make assertions on a cast from floating point 
                        goto DONE_ASSERTION;
                    }
          
                    switch (toType)
                    {
                    case TYP_BOOL:
                    case TYP_BYTE:
                    case TYP_UBYTE:
                    case TYP_SHORT:
                    case TYP_USHORT:
                    case TYP_CHAR:
#ifdef _TARGET_64BIT_
                    case TYP_UINT:
                    case TYP_INT:
#endif // _TARGET_64BIT_
                    assertion->op2.u2.loBound = AssertionDsc::GetLowerBoundForIntegralType(toType);
                    assertion->op2.u2.hiBound = AssertionDsc::GetUpperBoundForIntegralType(toType);
                    break;

                    default:
                        goto DONE_ASSERTION;  // Don't make an assertion
                    }
                    assertion->op2.kind = O2K_SUBRANGE;
                    assertion->assertionKind = OAK_SUBRANGE;
                }
                break;
            }
        } // else // !haveArgs
    } // if (op1->gtOper == GT_LCL_VAR)

    //
    // Are we making an IsType assertion? 
    // 
    else if (op1->gtOper == GT_IND)
    {
        op1 = op1->gtOp.gtOp1;
        //
        // Is this an indirection of a local variable?
        // 
        if (op1->gtOper == GT_LCL_VAR)
        {
            unsigned lclNum = op1->gtLclVarCommon.gtLclNum;    noway_assert(lclNum  < lvaCount);
            LclVarDsc * lclVar = &lvaTable[lclNum];

            //  If the local variable has its address exposed then bail 
            if (fgExcludeFromSsa(lclNum))
            {
                goto DONE_ASSERTION;
            }

            // If we have an typeHnd indirection then op1 must be a TYP_REF 
            //  and the indirection must produce a TYP_I
            //  
            if (op1->gtType != TYP_REF)
            {
                goto DONE_ASSERTION;  // Don't make an assertion
            }
    
            assertion->op1.kind = O1K_EXACT_TYPE;
            assertion->op1.lcl.lclNum = lclNum;
            assertion->op1.vn = op1->gtVNPair.GetConservative();
            assertion->op1.lcl.ssaNum = op1->AsLclVarCommon()->GetSsaNum();
            assert(assertion->op1.lcl.ssaNum == SsaConfig::RESERVED_SSA_NUM || assertion->op1.vn == lvaTable[lclNum].GetPerSsaData(assertion->op1.lcl.ssaNum)->m_vnPair.GetConservative());

            ssize_t cnsValue = 0;
            unsigned iconFlags = 0;
            // Ngen case
            if (op2->gtOper == GT_IND)
            {
                if (!optIsTreeKnownIntValue(!optLocalAssertionProp, op2->gtOp.gtOp1, &cnsValue, &iconFlags))
                {
                    goto DONE_ASSERTION;  // Don't make an assertion
                }

                assertion->assertionKind = assertionKind; 
                assertion->op2.kind = O2K_IND_CNS_INT;
                assertion->op2.u1.iconVal = cnsValue;
                assertion->op2.vn = op2->gtOp.gtOp1->gtVNPair.GetConservative();
                /* iconFlags should only contain bits in GTF_ICON_HDL_MASK */
                assert((iconFlags & ~GTF_ICON_HDL_MASK) == 0);
                assertion->op2.u1.iconFlags = iconFlags;
#ifdef _TARGET_64BIT_
                if (op2->gtOp.gtOp1->TypeGet() == TYP_LONG)
                    assertion->op2.u1.iconFlags |= 1; // Signify that this is really TYP_LONG
#endif // _TARGET_64BIT_
            }
            // JIT case
            else if (optIsTreeKnownIntValue(!optLocalAssertionProp, op2, &cnsValue, &iconFlags))
            {
                assertion->assertionKind = assertionKind; 
                assertion->op2.kind = O2K_IND_CNS_INT;
                assertion->op2.u1.iconVal = cnsValue;
                assertion->op2.vn = op2->gtVNPair.GetConservative();
                /* iconFlags should only contain bits in GTF_ICON_HDL_MASK */
                assert((iconFlags & ~GTF_ICON_HDL_MASK) == 0);
                assertion->op2.u1.iconFlags = iconFlags;
#ifdef _TARGET_64BIT_
                if (op2->TypeGet() == TYP_LONG)
                    assertion->op2.u1.iconFlags |= 1; // Signify that this is really TYP_LONG
#endif // _TARGET_64BIT_
            }
            else
            {
                goto DONE_ASSERTION; // Don't make an assertion
            }
        }
    }

DONE_ASSERTION:
    if (assertion->assertionKind == OAK_INVALID)
    {
        return NO_ASSERTION_INDEX;
    }

    if (!optLocalAssertionProp)
    {
        if (assertion->op1.vn == ValueNumStore::NoVN ||
            assertion->op2.vn == ValueNumStore::NoVN ||
            assertion->op1.vn == ValueNumStore::VNForVoid() ||
            assertion->op2.vn == ValueNumStore::VNForVoid() ||
            assertion->op1.lcl.ssaNum == SsaConfig::RESERVED_SSA_NUM)
        {
            return NO_ASSERTION_INDEX;
        }
    }

    // Now add the assertion to our assertion table
    noway_assert(assertion->op1.kind != O1K_INVALID);
    noway_assert(assertion->op1.kind == O1K_ARR_BND ||
                 assertion->op2.kind != O2K_INVALID);
    return optAddAssertion(assertion);
}

/*****************************************************************************
 *
 * If tree is a constant node holding an integral value, retrieve the value in
 * pConstant. If the method returns true, pConstant holds the appropriate
 * constant. Set "vnBased" to true to indicate local or global assertion prop.
 * "pFlags" indicates if the constant is a handle marked by GTF_ICON_HDL_MASK.
 */
bool Compiler::optIsTreeKnownIntValue(bool vnBased, GenTreePtr tree, ssize_t* pConstant, unsigned* pFlags)
{
    // Is Local assertion prop?
    if (!vnBased)
    {
        if (tree->OperGet() == GT_CNS_INT)
        {
            *pConstant = tree->gtIntCon.IconValue();
            *pFlags = tree->GetIconHandleFlag();
            return true;
        }
#ifdef _TARGET_64BIT_
        // Just to be clear, get it from gtLconVal rather than
        // overlapping gtIconVal.
        else if (tree->OperGet() == GT_CNS_LNG)
        {
            *pConstant = tree->gtLngCon.gtLconVal;
            *pFlags = tree->GetIconHandleFlag();
            return true;
        }
#endif
        return false;
    }

    // Global assertion prop
    if (!vnStore->IsVNConstant(tree->gtVNPair.GetConservative()))
    {
        return false;
    }

    ValueNum vn = tree->gtVNPair.GetConservative();
    var_types vnType = vnStore->TypeOfVN(vn);
    if (vnType == TYP_INT)
    {
        *pConstant = vnStore->ConstantValue<int>(vn);
        *pFlags = vnStore->IsVNHandle(vn) ? vnStore->GetHandleFlags(vn) : 0;
        return true;
    }
#ifdef _TARGET_64BIT_
    else if (vnType == TYP_LONG)
    {
        *pConstant = vnStore->ConstantValue<INT64>(vn);
        *pFlags = vnStore->IsVNHandle(vn) ? vnStore->GetHandleFlags(vn) : 0;
        return true;
    }
#endif
    return false;
}

/*****************************************************************************
 *
 * Maintain a map "optValueNumToAsserts" i.e., vn -> to set of assertions
 * about that VN. Given "assertions" about a "vn" add it to the previously
 * mapped assertions about that "vn."
 */
void Compiler::optAddVnAssertionMapping(ValueNum vn, const EXPSET_TP& assertions)
{
    EXPSET_TP cur = 0;
    optValueNumToAsserts->Lookup(vn, &cur);
    cur |= assertions;
    optValueNumToAsserts->Set(vn, cur);
}

/*****************************************************************************
 * Statically if we know that this assertion's VN involves a NaN don't bother
 * wasting an assertion table slot.
 */
bool  Compiler::optAssertionVnInvolvesNan(AssertionDsc* assertion)
{
    if (optLocalAssertionProp)
    {
        return false;
    }

    static const int SZ = 2;
    ValueNum vns[SZ] = { assertion->op1.vn, assertion->op2.vn };
    for (int i = 0; i < SZ; ++i)
    {
        if (vnStore->IsVNConstant(vns[i]))
        {
            var_types type = vnStore->TypeOfVN(vns[i]);
            if ((type == TYP_FLOAT  && _isnan(vnStore->ConstantValue<float >(vns[i])) != 0) ||
                (type == TYP_DOUBLE && _isnan(vnStore->ConstantValue<double>(vns[i])) != 0))
            {
                return true;
            }
        }
    }
    return false;
}

/*****************************************************************************
 *
 *  Given an assertion add it to the assertion table 
 * 
 *  If it is already in the assertion table return the assertionIndex that 
 *  we use to refer to this element.
 *  Otherwise add it to the assertion table ad return the assertionIndex that 
 *  we use to refer to this element.
 *  If we need to add to the table and the table is full return the value zero
 */  
unsigned Compiler::optAddAssertion(AssertionDsc* newAssertion)
{
    noway_assert(newAssertion->assertionKind != OAK_INVALID);

    // Even though the propagation step takes care of NaN, just a check
    // to make sure there is no slot involving a NaN.
    if (optAssertionVnInvolvesNan(newAssertion))
    {
        JITDUMP("Assertion involved Nan not adding\n");
        return NO_ASSERTION_INDEX;
    }


    // Check if exists already, so we can skip adding new one.
    for (unsigned index = 1; index <= optAssertionCount; index++)
    {
        AssertionDsc* curAssertion = optGetAssertion(index);
        if (curAssertion->Equals(newAssertion, !optLocalAssertionProp))
        {
            return index;
        }
    }

    // Check if we are within max count.
    if (optAssertionCount >= MAX_ASSERTION_CNT)
    {
        return NO_ASSERTION_INDEX;
    }

    optAssertionTabPrivate[optAssertionCount] = *newAssertion;
    optAssertionCount++;

#ifdef  DEBUG
    if  (verbose)
    {
        printf("GenTreeNode creates assertion:\n");
        gtDispTree(optAssertionPropCurrentTree, 0, nullptr, true);
        printf(optLocalAssertionProp ? "In BB%02u New Local " : "In BB%02u New Global ", compCurBB->bbNum);
        optPrintAssertion(newAssertion, optAssertionCount);
    }
#endif // DEBUG

    // Assertion mask bits are [index + 1].
    EXPSET_TP assertionBit = optGetAssertionBit(optAssertionCount);
    if (optLocalAssertionProp)
    {
        assert(newAssertion->op1.kind == O1K_LCLVAR);
        // Mark the variables this index depends on
        lvaTable[newAssertion->op1.lcl.lclNum].lvAssertionDep |= assertionBit;

        if (newAssertion->op2.kind == O2K_LCLVAR_COPY)
        {
            lvaTable[newAssertion->op2.lcl.lclNum].lvAssertionDep |= assertionBit;
        } 
    }
    else
    // If global assertion prop, then add it to the dependents map.
    {
        optAddVnAssertionMapping(newAssertion->op1.vn, assertionBit);
        if (newAssertion->op2.kind == O2K_LCLVAR_COPY)
        {
            optAddVnAssertionMapping(newAssertion->op2.vn, assertionBit);
        }
    }

#ifdef DEBUG
    optDebugCheckAssertions(optAssertionCount);
#endif
    return optAssertionCount;
}

#ifdef DEBUG
/*****************************************************************************
 *
 *  Verify that assertion prop related assumptions are valid. If "index"
 *  is 0 (i.e., NO_ASSERTION_INDEX) then verify all assertions in the table.
 *  If "index" is between 1 and optAssertionCount, then verify the assertion
 *  desc corresponding to "index."
 */
void Compiler::optDebugCheckAssertions(unsigned index)
{
    unsigned start = (index == NO_ASSERTION_INDEX) ? 1 : index;
    unsigned end = (index == NO_ASSERTION_INDEX) ? optAssertionCount : index;
    for (unsigned ind = start; ind <= end; ++ind)
    {
        AssertionDsc* assertion = optGetAssertion(ind);
        switch (assertion->op2.kind)
        {
        case O2K_IND_CNS_INT:
        case O2K_CONST_INT:
           {
               switch (assertion->op1.kind)
               {
               case O1K_EXACT_TYPE:
               case O1K_SUBTYPE:
                   assert(assertion->op2.u1.iconFlags != 0);
                   break;
               case O1K_ARRLEN_OPER_BND:
               case O1K_ARRLEN_LOOP_BND:
                   assert(!optLocalAssertionProp);
                   break;
               default:
                   assert(lvaTable[assertion->op1.lcl.lclNum].lvType != TYP_REF || assertion->op2.u1.iconVal == 0);
                   break;
               }
           }
           break;
        }
    }
}
#endif

/*****************************************************************************
 *
 * Given a "candidateAssertion", and the assertion operands op1 and op2,
 * create a complementary assertion and add it to the assertion table,
 * which can be retrieved using optFindComplementary(index)
 *
 */

void Compiler::optCreateComplementaryAssertion(const AssertionDsc& candidateAssertion, GenTreePtr op1, GenTreePtr op2)
{
    if (candidateAssertion.op1.kind == O1K_ARRLEN_OPER_BND ||
        candidateAssertion.op1.kind == O1K_ARRLEN_LOOP_BND)
    {
        AssertionDsc dsc = candidateAssertion;
        dsc.assertionKind = dsc.assertionKind == OAK_EQUAL ? OAK_NOT_EQUAL : OAK_EQUAL;
        optAddAssertion(&dsc);
        return;
    }

    if (candidateAssertion.assertionKind == OAK_EQUAL)
    {
        optCreateAssertion(op1, op2, OAK_NOT_EQUAL);
    }
    else if (candidateAssertion.assertionKind == OAK_NOT_EQUAL)
    {
        optCreateAssertion(op1, op2, OAK_EQUAL);
    }

    // Are we making a subtype or exact type assertion?
    if ((candidateAssertion.op1.kind == O1K_SUBTYPE) || (candidateAssertion.op1.kind == O1K_EXACT_TYPE))
    {
        // Did we recieve helper call args?
        if (op1->gtOper == GT_LIST)
        {
            op1 = op1->gtOp.gtOp1;
        }
        optCreateAssertion(op1, nullptr, OAK_NOT_EQUAL);
    }
}

/*****************************************************************************
 *
 * Create assertions for jtrue operands. Given operands "op1" and "op2" that
 * are used in a conditional evaluation of a jtrue stmt, create assertions
 * for the operands.
 */

unsigned Compiler::optCreateJtrueAssertions(GenTreePtr op1, GenTreePtr op2, Compiler::optAssertionKind assertionKind)
{
    AssertionDsc candidateAssertion;
    unsigned assertionIndex = optCreateAssertion(op1, op2, assertionKind, &candidateAssertion);
    // Don't bother if we don't have an assertion on the JTrue False path. Current implementation
    // allows for a complementary only if there is an assertion on the False path (tree->HasAssertion()).
    if (assertionIndex != NO_ASSERTION_INDEX)
    {
        optCreateComplementaryAssertion(candidateAssertion, op1, op2);
    }
    return assertionIndex;
}

unsigned Compiler::optCreateJTrueArrayAssertion(GenTreePtr tree)
{
    GenTreePtr relop = tree->gtGetOp1();
    if ((relop->OperKind() & GTK_RELOP) == 0)
    {
        return NO_ASSERTION_INDEX;
    }
    GenTreePtr op1 = relop->gtGetOp1();
    GenTreePtr op2 = relop->gtGetOp2();

    ValueNum vn = op1->gtVNPair.GetConservative();
    // Cases where op1 holds the condition with array arithmetic and op2 is 0.
    // Loop condition like: "i < a.len +/-k == 0"
    // Assertion: "i < a.len +/- k == 0"
    if (vnStore->IsVNArrLenArithBound(vn) && 
        op2->gtVNPair.GetConservative() == vnStore->VNZeroForType(op2->TypeGet()) &&
        (relop->gtOper == GT_EQ || relop->gtOper == GT_NE))
    {
        AssertionDsc dsc;
        dsc.assertionKind = relop->gtOper == GT_EQ ? OAK_EQUAL : OAK_NOT_EQUAL;
        dsc.op1.kind = O1K_ARRLEN_OPER_BND;
        dsc.op1.vn = vn;
        dsc.op2.kind = O2K_CONST_INT;
        dsc.op2.vn = vnStore->VNZeroForType(op2->TypeGet());
        dsc.op2.u1.iconVal = 0;
        dsc.op2.u1.iconFlags = 0;
        unsigned index = optAddAssertion(&dsc);
        optCreateComplementaryAssertion(dsc, nullptr, nullptr);
        return index;
    }
    // Cases where op1 holds the condition array length and op2 is 0.
    // Loop condition like: "i < a.len == 0"
    // Assertion: "i < a.len == false"
    else if (vnStore->IsVNArrLenBound(vn) && 
        (op2->gtVNPair.GetConservative() == vnStore->VNZeroForType(op2->TypeGet())) &&
        (relop->gtOper == GT_EQ || relop->gtOper == GT_NE))
    {
        AssertionDsc dsc;
        dsc.assertionKind = relop->gtOper == GT_EQ ? OAK_EQUAL : OAK_NOT_EQUAL;
        dsc.op1.kind = O1K_ARRLEN_LOOP_BND;
        dsc.op1.vn = vn;
        dsc.op2.kind = O2K_CONST_INT;
        dsc.op2.vn = vnStore->VNZeroForType(op2->TypeGet());
        dsc.op2.u1.iconVal = 0;
        dsc.op2.u1.iconFlags = 0;
        unsigned index = optAddAssertion(&dsc);
        optCreateComplementaryAssertion(dsc, nullptr, nullptr);
        return index;
    }
    // Cases where op1 holds the lhs of the condition op2 holds rhs.
    // Loop condition like "i < a.len"
    // Assertion: "i < a.len != 0"
    else if (vnStore->IsVNArrLenBound(relop->gtVNPair.GetConservative()))
    {
        AssertionDsc dsc;
        dsc.assertionKind = OAK_NOT_EQUAL;
        dsc.op1.kind = O1K_ARRLEN_LOOP_BND;
        dsc.op1.vn = relop->gtVNPair.GetConservative();
        dsc.op2.kind = O2K_CONST_INT;
        dsc.op2.vn = vnStore->VNZeroForType(TYP_INT);
        dsc.op2.u1.iconVal = 0;
        dsc.op2.u1.iconFlags = 0;
        unsigned index = optAddAssertion(&dsc);
        optCreateComplementaryAssertion(dsc, nullptr, nullptr);
        return index;
    }
    return NO_ASSERTION_INDEX;
}

/*****************************************************************************
 *
 *  Compute assertions for the JTrue node.
 */
unsigned Compiler::optAssertionGenJtrue(GenTreePtr tree)
{
    // Only create assertions for JTRUE when we are in the global phase
    if (optLocalAssertionProp)
    {
        return NO_ASSERTION_INDEX;
    }

    GenTreePtr relop = tree->gtOp.gtOp1;
    if ((relop->OperKind() & GTK_RELOP) == 0)
    {
        return NO_ASSERTION_INDEX;
    }

    Compiler::optAssertionKind assertionKind = OAK_INVALID;

    GenTreePtr op1 = relop->gtOp.gtOp1;
    GenTreePtr op2 = relop->gtOp.gtOp2;

    unsigned index = optCreateJTrueArrayAssertion(tree);
    if (index != NO_ASSERTION_INDEX)
    {
        return index;
    }

    // Find assertion kind.
    switch (relop->gtOper)
    {
    case GT_EQ:
        assertionKind = OAK_EQUAL;
        break;
    case GT_NE:
        assertionKind = OAK_NOT_EQUAL;
        break;
    default:
        // TODO-CQ: add other relop operands. Disabled for now to measure perf
        // and not occupy assertion table slots. We'll add them when used.
        return NO_ASSERTION_INDEX;
    }

    // Check for op1 or op2 to be lcl var and if so, keep it in op1.
    if ((op1->gtOper != GT_LCL_VAR) && (op2->gtOper == GT_LCL_VAR))
    {
        jitstd::swap(op1, op2);
    }
    // If op1 is lcl and op2 is const or lcl, create assertion.
    if ((op1->gtOper == GT_LCL_VAR) && 
        ((op2->OperKind() & GTK_CONST) || (op2->gtOper == GT_LCL_VAR)))  // Fix for Dev10 851483
    {
        return optCreateJtrueAssertions(op1, op2, assertionKind);
    }

    // Check op1 and op2 for an indirection of a GT_LCL_VAR and keep it in op1.
    if (((op1->gtOper != GT_IND) || (op1->gtOp.gtOp1->gtOper != GT_LCL_VAR)) &&
        ((op2->gtOper == GT_IND) && (op2->gtOp.gtOp1->gtOper == GT_LCL_VAR)))
    {
        jitstd::swap(op1, op2);
    }
    // If op1 is ind, then extract op1's oper.
    if ((op1->gtOper == GT_IND) && (op1->gtOp.gtOp1->gtOper == GT_LCL_VAR))
    {
        return optCreateJtrueAssertions(op1, op2, assertionKind);
    }

    // Look for a call to an IsInstanceOf helper compared to a nullptr
    if ((op2->gtOper != GT_CNS_INT) && (op1->gtOper == GT_CNS_INT))
    {
        jitstd::swap(op1, op2);
    }
    // Validate op1 and op2
    if ((op1->gtOper != GT_CALL) || (op1->gtCall.gtCallType != CT_HELPER) || (op1->TypeGet() != TYP_REF) || // op1
        (op2->gtOper != GT_CNS_INT) || (op2->gtIntCon.gtIconVal != 0))                                      // op2
    {
        return NO_ASSERTION_INDEX;
    }
    if (op1->gtCall.gtCallMethHnd != eeFindHelper(CORINFO_HELP_ISINSTANCEOFINTERFACE) &&
        op1->gtCall.gtCallMethHnd != eeFindHelper(CORINFO_HELP_ISINSTANCEOFARRAY) &&
        op1->gtCall.gtCallMethHnd != eeFindHelper(CORINFO_HELP_ISINSTANCEOFCLASS) &&
        op1->gtCall.gtCallMethHnd != eeFindHelper(CORINFO_HELP_ISINSTANCEOFANY))
    {
        return NO_ASSERTION_INDEX;
    }
                    
    op2 = op1->gtCall.gtCallLateArgs->gtOp.gtOp2;
    op1 = op1->gtCall.gtCallLateArgs;

    // Reverse the assertion
    assert(assertionKind == OAK_EQUAL || assertionKind == OAK_NOT_EQUAL);
    assertionKind = (assertionKind == OAK_EQUAL) ? OAK_NOT_EQUAL : OAK_EQUAL;

    if (op1->gtOp.gtOp1->gtOper == GT_LCL_VAR)
    {
        return optCreateJtrueAssertions(op1, op2, assertionKind);
    }

    return NO_ASSERTION_INDEX;
}

/*****************************************************************************
 *
 *  Create an assertion on the phi node if some information can be gleaned
 *  from all of the constituent phi operands.
 *
 */
unsigned Compiler::optAssertionGenPhiDefn(GenTreePtr tree)
{
    if (!tree->IsPhiDefn())
    {
        return NO_ASSERTION_INDEX;
    }

    GenTreePtr phi = tree->gtOp.gtOp2;

    // Try to find if all phi arguments are known to be non-null.
    bool isNonNull = true;
    for (GenTreeArgList* args = phi->gtOp.gtOp1->AsArgList();
         args != nullptr;
         args = args->Rest())
    {
        if (!vnStore->IsKnownNonNull(args->Current()->gtVNPair.GetConservative()))
        {
            isNonNull = false;
            break;
        }
    }

    // All phi arguments are non-null implies phi rhs is non-null.
    if (isNonNull)
    {
        return optCreateAssertion(tree->gtOp.gtOp1, nullptr, OAK_NOT_EQUAL);
    }
    return NO_ASSERTION_INDEX;
}

/*****************************************************************************
 *
 *  If this statement creates a value assignment or assertion
 *  then assign an index to the given value assignment by adding
 *  it to the lookup table, if necessary. 
 */
void Compiler::optAssertionGen(GenTreePtr tree) 
{
    tree->ClearAssertion();

    if (tree->gtFlags & GTF_COLON_COND)
    {
        return;
    }

#ifdef DEBUG
    optAssertionPropCurrentTree = tree;
#endif

    // For most of the assertions that we create below 
    // the assertion is true after the tree is processed
    bool assertionProven = true;
    unsigned assertionIndex  = 0;  
    switch (tree->gtOper)
    {
    case GT_ASG:
        // VN takes care of non local assertions for assignments and data flow.
        if (optLocalAssertionProp)
        {
            assertionIndex = optCreateAssertion(tree->gtOp.gtOp1, tree->gtOp.gtOp2, OAK_EQUAL);
        }
        else
        {
            assertionIndex = optAssertionGenPhiDefn(tree);
        }
        break;

    case GT_IND:
    case GT_NULLCHECK:
        // An indirection can create a non-null assertion
    case GT_ARR_LENGTH:
        // An array length can create a non-null assertion
        assertionIndex = optCreateAssertion(tree->gtOp.gtOp1, nullptr, OAK_NOT_EQUAL);
        break;

    case GT_ARR_BOUNDS_CHECK:
        if (!optLocalAssertionProp)
        {
            assertionIndex = optCreateAssertion(tree, nullptr, OAK_NO_THROW);
        }
        break;

    case GT_ARR_ELEM:
        // An array element reference can create a non-null assertion
        assertionIndex = optCreateAssertion(tree->gtArrElem.gtArrObj, nullptr, OAK_NOT_EQUAL);
        break;

    case GT_CALL:
        // A virtual call can create a non-null assertion. We transform some virtual calls into non-virtual calls
        // with a GTF_CALL_NULLCHECK flag set.
        if ((tree->gtFlags & GTF_CALL_NULLCHECK) ||
            ((tree->gtFlags & GTF_CALL_VIRT_KIND_MASK) != GTF_CALL_NONVIRT))
        {
            //  Retrieve the 'this' arg
            GenTreePtr thisArg = gtGetThisArg(tree);
#if defined(_TARGET_AMD64_) || defined(_TARGET_ARM_)
            if (thisArg == nullptr)
            {
                // For tail calls we lose the this pointer in the argument list but that's OK because a null check
                // was made explicit, so we get the assertion when we walk the GT_IND in the argument list.
                noway_assert(tree->gtCall.IsTailCall());
                break;
            }
#endif // _TARGET_AMD64_ || _TARGET_ARM_
            noway_assert(thisArg != nullptr);
            assertionIndex = optCreateAssertion(thisArg, nullptr, OAK_NOT_EQUAL);
        }
        break;

    case GT_CAST:
        // We only create this assertion for global assertion prop
        if (!optLocalAssertionProp)
        {
            // This represets an assertion that we would like to prove to be true. It is not actually a true assertion.
            // If we can prove this assertion true then we can eliminate this cast.
            assertionIndex = optCreateAssertion(tree->gtOp.gtOp1, tree, OAK_SUBRANGE);
            assertionProven = false;  
        }
        break;

    case GT_JTRUE:
        assertionIndex = optAssertionGenJtrue(tree);
        break;
    }

    // For global assertion prop we must store the assertion number in the tree node
    if ((assertionIndex != NO_ASSERTION_INDEX) && assertionProven && !optLocalAssertionProp)
    {
        tree->SetAssertion(assertionIndex);
    }
}

/*****************************************************************************
 *
 *  Given an assertion index, return the assertion index of the complementary 
 *  assertion or 0 if one does not exist.
 */  
unsigned Compiler::optFindComplementary(unsigned assertIndex)
{
    if (assertIndex == NO_ASSERTION_INDEX)
    {
        return NO_ASSERTION_INDEX;
    }
    AssertionDsc* inputAssertion = optGetAssertion(assertIndex);

    // Must be an equal or not equal assertion.
    if (inputAssertion->assertionKind != OAK_EQUAL && inputAssertion->assertionKind != OAK_NOT_EQUAL)
    {
        return NO_ASSERTION_INDEX;
    }

    optAssertionKind complementaryAssertionKind = (inputAssertion->assertionKind == OAK_EQUAL) ? OAK_NOT_EQUAL : OAK_EQUAL;
    for (unsigned index = 1; index <= optAssertionCount; ++index)
    {
        // Make sure assertion kinds are complementary and op1, op2 kinds match.
        AssertionDsc* curAssertion = optGetAssertion(index);
        if (curAssertion->Complementary(inputAssertion, !optLocalAssertionProp))
        {
            return index;
        }
    }
    return NO_ASSERTION_INDEX;
}

/*****************************************************************************
 *
 *  Given a lclNum and a toType, return assertion index of the assertion that
 *  claims that a variable's value is always a valid subrange of toType.
 *  Thus we can discard or omit a cast to toType. Returns NO_ASSERTION_INDEX
 *  if one such assertion could not be found in "assertions."
 */

unsigned Compiler::optAssertionIsSubrange(GenTreePtr tree, var_types toType, EXPSET_TP assertions)
{
    if (!optLocalAssertionProp && assertions == 0)
    {
        return NO_ASSERTION_INDEX;
    }

    EXPSET_TP mask = 1;
    for (unsigned index = 1; index <= optAssertionCount; index++, mask <<= 1)
    {
        noway_assert(mask == optGetAssertionBit(index));

        AssertionDsc* curAssertion = optGetAssertion(index);
        if  ((optLocalAssertionProp || (assertions & mask)) &&  // either local prop or use propagated assertions
             (curAssertion->assertionKind == OAK_SUBRANGE) && 
             (curAssertion->op1.kind == O1K_LCLVAR))
        {
            // For local assertion prop use comparison on locals, and use comparison on vns for global prop.
            bool isEqual = optLocalAssertionProp ? (curAssertion->op1.lcl.lclNum == tree->AsLclVarCommon()->GetLclNum())
                                                 : (curAssertion->op1.vn == tree->gtVNPair.GetConservative());
            if (!isEqual)
            {
                continue;
            }

            // Make sure the toType is within current assertion's bounds.
            switch (toType)
            {
            case TYP_BYTE:
            case TYP_UBYTE:
            case TYP_SHORT:
            case TYP_USHORT:
            case TYP_CHAR:
                if ((curAssertion->op2.u2.loBound < AssertionDsc::GetLowerBoundForIntegralType(toType)) ||
                    (curAssertion->op2.u2.hiBound > AssertionDsc::GetUpperBoundForIntegralType(toType)))
                {
                    continue;
                }
                break;

            case TYP_UINT:
                if (curAssertion->op2.u2.loBound < AssertionDsc::GetLowerBoundForIntegralType(toType))
                {
                    continue;
                }
                break;

            case TYP_INT:
                break;

            default:
                continue;
            }
            return index;
        }
    }
    return NO_ASSERTION_INDEX;
}

/**********************************************************************************
 *
 * Given a "tree" that is usually arg1 of a isinst/cast kind of GT_CALL (a class
 * handle), and "methodTableArg" which is a const int (a class handle), then search
 * if there is an assertion in "assertions", that asserts the equality of the two
 * class handles and then returns the index of the assertion. If one such assertion
 * could not be found, then it returns NO_ASSERTION_INDEX.
 *
 */
unsigned Compiler::optAssertionIsSubtype(GenTreePtr tree, GenTreePtr methodTableArg, EXPSET_TP assertions)
{
    if (!optLocalAssertionProp && assertions == 0)
    {
        return NO_ASSERTION_INDEX;
    }
    EXPSET_TP mask = 1;
    for (unsigned index = 1; index <= optAssertionCount; index++, mask <<= 1)
    {
        assert(mask == optGetAssertionBit(index));

        if  (!optLocalAssertionProp && !(assertions & mask))
        {
            continue;
        }

        AssertionDsc* curAssertion = optGetAssertion(index);
        if (curAssertion->assertionKind != OAK_EQUAL ||
            (curAssertion->op1.kind != O1K_SUBTYPE &&
             curAssertion->op1.kind != O1K_EXACT_TYPE))
        {
            continue;
        }

        // If local assertion prop use "lcl" based comparison, if global assertion prop use vn based comparison.
        if ((optLocalAssertionProp) ? (curAssertion->op1.lcl.lclNum != tree->AsLclVarCommon()->GetLclNum())
                                    : (curAssertion->op1.vn != tree->gtVNPair.GetConservative()))
        {
            continue;
        }

        if (curAssertion->op2.kind == O2K_IND_CNS_INT)
        {
            if (methodTableArg->gtOper != GT_IND)
            {
                continue;
            }
            methodTableArg = methodTableArg->gtOp.gtOp1;
        }
        else if (curAssertion->op2.kind != O2K_CONST_INT)
        {
            continue;
        }
        
        ssize_t methodTableVal = 0;
        unsigned iconFlags = 0;
        if (!optIsTreeKnownIntValue(!optLocalAssertionProp, methodTableArg, &methodTableVal, &iconFlags))
        {
            continue;
        }

        if (curAssertion->op2.u1.iconVal == methodTableVal)
        {
            return index;
        }
    }
    return NO_ASSERTION_INDEX;
}

/*******************************************************************************************************
 *
 * Perform value numbering based constant propagation on the tree node. Given a "tree" that is identified
 * by value numbering system as having a "constant vn," then substitute the given node by the constant
 * of the correct type.
 *
 */
GenTreePtr Compiler::optVnConstantAssertionProp(const GenTreePtr tree, const GenTreePtr stmt)
{
    assert(!optLocalAssertionProp);

    if (!vnStore->IsVNConstant(tree->gtVNPair.GetConservative()))
    {
        return nullptr;
    }

    // Don't propagate floating-point constants into a TYP_STRUCT LclVar
    // This can occur for HFA return values (see hfa_sf3E_r.exe)
    //
    if (tree->TypeGet() == TYP_STRUCT)
    {
        return nullptr;
    }

#ifdef DEBUG
    if  (verbose)
    {
        printf("\nVN based constant assertion prop in BB%02u:\n", compCurBB->bbNum);
        printf("Old Tree:   ");
        gtDispTree(tree, 0, nullptr, true);
    }
#endif
        
    ValueNum   vnCns   = tree->gtVNPair.GetConservative();  // Save the ValueNum
    GenTreePtr newTree = tree;

    switch (vnStore->TypeOfVN(tree->gtVNPair.GetConservative()))
    {
    case TYP_FLOAT:
        lvaRecursiveDecRefCounts(newTree);
        newTree->ChangeOperConst(GT_CNS_DBL);
        newTree->gtDblCon.gtDconVal = vnStore->ConstantValue<float>(vnCns);
        break;

    case TYP_DOUBLE:
        lvaRecursiveDecRefCounts(newTree);
        newTree->ChangeOperConst(GT_CNS_DBL);
        newTree->gtDblCon.gtDconVal = vnStore->ConstantValue<double>(vnCns);
        break;

    case TYP_LONG:
        {
            INT64 value = vnStore->ConstantValue<INT64>(vnCns);
#ifdef _TARGET_64BIT_           
            if (vnStore->IsVNHandle(vnCns))
            {
                lvaRecursiveDecRefCounts(newTree);
                newTree = gtNewIconHandleNode(value, vnStore->GetHandleFlags(vnCns));
            }
            else
#endif
            {
                switch (tree->TypeGet())
                {
                case TYP_INT:
                    lvaRecursiveDecRefCounts(newTree);
                    newTree->ChangeOperConst(GT_CNS_INT);
                    newTree->gtIntCon.gtIconVal = (int) value;
                    break;
                    
                case TYP_LONG:
                    lvaRecursiveDecRefCounts(newTree);
                    newTree->ChangeOperConst(GT_CNS_NATIVELONG);
                    newTree->gtIntConCommon.SetLngValue(value);
                    break;
                    
                case TYP_FLOAT:
                    lvaRecursiveDecRefCounts(newTree);
                    newTree->ChangeOperConst(GT_CNS_DBL);
                    newTree->gtDblCon.gtDconVal = (float) value;
                    break;
                    
                case TYP_DOUBLE:
                    lvaRecursiveDecRefCounts(newTree);
                    newTree->ChangeOperConst(GT_CNS_DBL);
                    newTree->gtDblCon.gtDconVal = (double) value;
                    break;
                    
                default:
                    return nullptr;
                }
            }
        }
        break;

    case TYP_REF:
        if (tree->TypeGet() != TYP_REF)
            return nullptr;

        assert(vnStore->ConstantValue<size_t>(vnCns) == 0);
        lvaRecursiveDecRefCounts(newTree);
        newTree->ChangeOperConst(GT_CNS_INT);
        newTree->gtIntCon.gtIconVal = 0;
        newTree->ClearIconHandleMask();
        break;

    case TYP_INT:
        {
            int value = vnStore->ConstantValue<int>(vnCns);
#ifndef _TARGET_64BIT_
            if (vnStore->IsVNHandle(vnCns))
            {
                lvaRecursiveDecRefCounts(newTree);
                newTree = gtNewIconHandleNode(value, vnStore->GetHandleFlags(vnCns));
            }
            else
#endif
            {
                switch (tree->TypeGet())
                {
                case TYP_REF:
                case TYP_INT:
                    lvaRecursiveDecRefCounts(newTree);
                    newTree->ChangeOperConst(GT_CNS_INT);
                    newTree->gtIntCon.gtIconVal = value;
                    newTree->ClearIconHandleMask();
                    break;
                    
                case TYP_LONG:
                    lvaRecursiveDecRefCounts(newTree);
                    newTree->ChangeOperConst(GT_CNS_NATIVELONG);
                    newTree->gtIntConCommon.SetLngValue((INT64) value);
                    break;
                    
                case TYP_FLOAT:
                    lvaRecursiveDecRefCounts(newTree);
                    newTree->ChangeOperConst(GT_CNS_DBL);
                    newTree->gtDblCon.gtDconVal = (float) value;
                    break;
                    
                case TYP_DOUBLE:
                    lvaRecursiveDecRefCounts(newTree);
                    newTree->ChangeOperConst(GT_CNS_DBL);
                    newTree->gtDblCon.gtDconVal = (double) value;
                    break;
                    
                default:
                    return nullptr;
                }
            }
        }
        break;

    default:
        return nullptr;
    }

    newTree->gtVNPair.SetBoth(vnCns);      // Preserve the ValueNumPair, as ChangeOperConst/SetOper will clear it.

#ifdef DEBUG
    if  (verbose)
    {
        printf("New Tree:   ");
        gtDispTree(newTree, 0, nullptr, true);
    }
#endif
    return optAssertionProp_Update(newTree, tree, stmt);
}

/*******************************************************************************************************
 *
 * Perform constant propagation on a tree given the "curAssertion" is true at the point of the "tree."
 *
 */
GenTreePtr Compiler::optConstantAssertionProp(AssertionDsc* curAssertion, GenTreePtr tree, GenTreePtr stmt DEBUGARG(unsigned index))
{
    unsigned lclNum = tree->gtLclVarCommon.gtLclNum;

    if (lclNumIsCSE(lclNum))
    {
        return nullptr;
    }

    GenTreePtr newTree = tree;

    // Update 'newTree' with the new value from our table
    // Typically newTree == tree and we are updating the node in place
    switch (curAssertion->op2.kind)
    {
    case O2K_CONST_DOUBLE:
        // There could be a positive zero and a negative zero, so don't propagate zeroes.
        if (curAssertion->op2.dconVal == 0.0)
        {
            return nullptr;
        }
        newTree->ChangeOperConst(GT_CNS_DBL);
        newTree->gtDblCon.gtDconVal = curAssertion->op2.dconVal;
        break;

    case O2K_CONST_LONG:
        if (newTree->gtType == TYP_LONG)
        {
            newTree->ChangeOperConst(GT_CNS_NATIVELONG);
            newTree->gtIntConCommon.SetLngValue(curAssertion->op2.lconVal);
        }
        else
        {
            newTree->ChangeOperConst(GT_CNS_INT);
            newTree->gtIntCon.gtIconVal = (int) curAssertion->op2.lconVal;                        
            newTree->gtType=TYP_INT;
        }
        break;

    case O2K_CONST_INT:
        if (curAssertion->op2.u1.iconFlags & GTF_ICON_HDL_MASK)
        {
            // Here we have to allocate a new 'large' node to replace the old one
            newTree = gtNewIconHandleNode(curAssertion->op2.u1.iconVal, 
                                            curAssertion->op2.u1.iconFlags & GTF_ICON_HDL_MASK);
        }
        else
        {
            bool isArrIndex = ((tree->gtFlags & GTF_VAR_ARR_INDEX) != 0);
            newTree->ChangeOperConst(GT_CNS_INT);
            newTree->gtIntCon.gtIconVal = curAssertion->op2.u1.iconVal;
            newTree->ClearIconHandleMask();
            // If we're doing an array index address, assume any constant propagated contributes to the index.
            if (isArrIndex)
            {
                newTree->gtIntCon.gtFieldSeq = GetFieldSeqStore()->CreateSingleton(FieldSeqStore::ConstantIndexPseudoField);
            }
            newTree->gtFlags &= ~GTF_VAR_ARR_INDEX;
        }

        // Constant ints are of type TYP_INT, not any of the short forms.
        if (varTypeIsIntegral(newTree->TypeGet()))
        {
#ifdef _TARGET_64BIT_
            var_types newType = (var_types)((curAssertion->op2.u1.iconFlags & 1) ? TYP_LONG : TYP_INT);
            if (newTree->TypeGet() != newType)
            {
                noway_assert(newTree->gtType != TYP_REF);
                newTree->gtType = newType;
            }
#else
            if (newTree->TypeGet() != TYP_INT)
            {
                noway_assert(newTree->gtType != TYP_REF && newTree->gtType != TYP_LONG);
                newTree->gtType = TYP_INT;
            }
#endif
        }
        break;
        
    default:
        return nullptr;
    }

    if (!optLocalAssertionProp)
    {
        assert(newTree->OperIsConst());                       // We should have a simple Constant node for newTree
        assert(vnStore->IsVNConstant(curAssertion->op2.vn));  // The value number stored for op2 should be a valid VN representing the constant
        newTree->gtVNPair.SetBoth(curAssertion->op2.vn);      // Set the ValueNumPair to the constant VN from op2 of the assertion
    }

#ifdef  DEBUG
    if (verbose)
    {
        printf("\nAssertion prop in BB%02u:\n", compCurBB->bbNum);
        optPrintAssertion(curAssertion, index);
        gtDispTree(newTree, 0, nullptr, true);
    }
#endif
    if (lvaLocalVarRefCounted)
    {
        lvaTable[lclNum].decRefCnts(compCurBB->getBBWeight(this), this);
    }

    return optAssertionProp_Update(newTree, tree, stmt);
}

/*******************************************************************************************************
 *
 *  Called in the context of an existing copy assertion which makes an "==" assertion on "lclVar" and
 *  "copyVar." Before substituting "copyVar" for "lclVar", we make sure using "copy" doesn't widen access.
 *
 */
bool Compiler::optAssertionProp_LclVarTypeCheck(GenTreePtr tree, LclVarDsc* lclVarDsc, LclVarDsc* copyVarDsc)
{
    /* 
        Small struct field locals are stored using the exact width and loaded widened 
        (i.e. lvNormalizeOnStore==false   lvNormalizeOnLoad==true),
        because the field locals might end up embedded in the parent struct local with the exact width.
                   
            In other words, a store to a short field local should always done using an exact width store
                   
                [00254538] 0x0009 ------------               const     int    0x1234
            [002545B8] 0x000B -A--G--NR---               =         short
                [00254570] 0x000A D------N----               lclVar    short  V43 tmp40

            mov   word  ptr [L_043], 0x1234                       
 
        Now, if we copy prop, say a short field local V43, to another short local V34
        for the following tree:

                [04E18650] 0x0001 ------------               lclVar    int   V34 tmp31
            [04E19714] 0x0002 -A----------               =         int
                [04E196DC] 0x0001 D------N----               lclVar    int   V36 tmp33

        We will end with this tree:
     
                [04E18650] 0x0001 ------------               lclVar    int   V43 tmp40
            [04E19714] 0x0002 -A-----NR---               =         int
                [04E196DC] 0x0001 D------N----               lclVar    int   V36 tmp33    EAX

        And eventually causing a fetch of 4-byte out from [L_043] :( 
            mov     EAX, dword ptr [L_043]

        The following check is to make sure we only perform the copy prop 
        when we don't retrieve the wider value.
    */       

    if (copyVarDsc->lvIsStructField)
    {
        var_types varType = (var_types) copyVarDsc->lvType;
        // Make sure we don't retrieve the wider value.
        return !varTypeIsSmall(varType) || (varType == tree->TypeGet());
    }
    // Called in the context of a single copy assertion, so the types should have been
    // taken care by the assertion gen logic for other cases. Just return true.
    return true;
}

/**********************************************************************************
 *
 *  Perform copy assertion propagation when the lclNum and ssaNum of the "tree" match
 *  the "curAssertion."
 *
 */
GenTreePtr Compiler::optCopyAssertionProp(AssertionDsc* curAssertion, GenTreePtr tree, GenTreePtr stmt DEBUGARG(unsigned index))
{
    const AssertionDsc::AssertionDscOp1& op1 = curAssertion->op1;
    const AssertionDsc::AssertionDscOp2& op2 = curAssertion->op2;

    noway_assert(op1.lcl.lclNum != op2.lcl.lclNum);

    unsigned lclNum = tree->gtLclVarCommon.GetLclNum();

    // Make sure one of the lclNum of the assertion matches with that of the tree.
    if (op1.lcl.lclNum != lclNum && op2.lcl.lclNum != lclNum)
    {
        return nullptr;
    }

    // Extract the matching lclNum and ssaNum.
    unsigned copyLclNum = (op1.lcl.lclNum == lclNum) ? op2.lcl.lclNum : op1.lcl.lclNum;
    unsigned copySsaNum = BAD_VAR_NUM;
    if (!optLocalAssertionProp)
    {
        // Extract the ssaNum of the matching lclNum.
        unsigned ssaNum = (op1.lcl.lclNum == lclNum) ? op1.lcl.ssaNum : op2.lcl.ssaNum;
        copySsaNum      = (op1.lcl.lclNum == lclNum) ? op2.lcl.ssaNum : op1.lcl.ssaNum;
        
        if (ssaNum != tree->AsLclVarCommon()->GetSsaNum())
        {
            return nullptr;
        }
    }

    LclVarDsc* copyVarDsc = &lvaTable[copyLclNum];
    LclVarDsc* lclVarDsc = &lvaTable[lclNum];

    // Make sure the types are compatible.
    if (!optAssertionProp_LclVarTypeCheck(tree, lclVarDsc, copyVarDsc))
    {
        return nullptr;
    }

    // Make sure we can perform this copy prop.
    if (optCopyProp_LclVarScore(lclVarDsc, copyVarDsc, curAssertion->op1.lcl.lclNum == lclNum) <= 0)
    {
        return nullptr;
    }

    // If global assertion prop, by now we should have ref counts, fix them.
    if (lvaLocalVarRefCounted)
    {
        lvaTable[lclNum].decRefCnts(compCurBB->getBBWeight(this), this);
        lvaTable[copyLclNum].incRefCnts(compCurBB->getBBWeight(this), this);
        tree->gtLclVarCommon.SetSsaNum(copySsaNum);
    }
    tree->gtLclVarCommon.SetLclNum(copyLclNum);

#ifdef  DEBUG
    if (verbose)
    {
        printf("\nAssertion prop in BB%02u:\n", compCurBB->bbNum);
        optPrintAssertion(curAssertion, index);
        gtDispTree(tree, 0, nullptr, true);
    }
#endif

    // Update and morph the tree.
    return optAssertionProp_Update(tree, tree, stmt);
}


/*****************************************************************************
 *
 *  Given a tree consisting of a just a LclVar and a set of available assertions
 *  we try to propagate an assertion and modify the LclVar tree if we can.
 *  We pass in the root of the tree via 'stmt', for local copy prop 'stmt' will
 *  be nullptr. Returns the modified tree, or nullptr if no assertion prop took place.
 */

GenTreePtr Compiler::optAssertionProp_LclVar(EXPSET_TP assertions, const GenTreePtr tree, const GenTreePtr stmt)
{
    assert(tree->gtOper == GT_LCL_VAR);
    // If we have a var definition then bail or
    // If this is the address of the var then it will have the GTF_DONT_CSE
    // flag set and we don't want to to assertion prop on it.
    if (tree->gtFlags & (GTF_VAR_DEF | GTF_DONT_CSE))
    {
        return nullptr;
    }

    // If global assertion prop, perform value numbering based constant prop.
    if (!optLocalAssertionProp)
    {
#if FEATURE_ANYCSE
        // Don't perform constant prop for CSE LclVars
        if (!lclNumIsCSE(tree->AsLclVarCommon()->GetLclNum()))
#endif
        {
            GenTreePtr newTree = optVnConstantAssertionProp(tree, stmt);
            if (newTree != nullptr)
            {
                return newTree;
            }
        }
    }

    // Check each assertion to see if it can be applied here.
    EXPSET_TP mask = 1;
    for (unsigned index = 1; index <= optAssertionCount; index++, mask <<= 1)
    {
        // See if the variable is equal to a constant or another variable.
        AssertionDsc* curAssertion = optGetAssertion(index);
        if (curAssertion->assertionKind != OAK_EQUAL || curAssertion->op1.kind != O1K_LCLVAR)
        {
            continue; 
        }

        // If the current assertion is not in the set of assertions, continue.
        if (!(mask & assertions))
        {
            continue;
        }

        // Copy prop.
        if (curAssertion->op2.kind == O2K_LCLVAR_COPY)
        {
            // Cannot do copy prop during global assertion prop because of no knowledge
            // of kill sets. We will still make a == b copy assertions during the global phase to allow
            // for any implied assertions that can be retrieved. Because implied assertions look for
            // matching SSA numbers (i.e., if a0 == b1 and b1 == c0 then a0 == c0) they don't need kill sets.
            if (optLocalAssertionProp)
            {
                // Perform copy assertion prop.
                GenTreePtr newTree = optCopyAssertionProp(curAssertion, tree, stmt DEBUGARG(index));
                if (newTree == nullptr)
                {
                    // Skip and try next assertion.
                    continue;
                }
                return newTree;
            }
        }
        // Constant prop (for local assertion prop.)
        // The case where the tree type could be different than the LclVar type is caused by
        // gtFoldExpr, specifically the case of a cast, where the fold operation changes the type of the LclVar 
        // node.  In such a case is not safe to perform the substitution since later on the JIT will assert mismatching
        // types between trees.
        else if (curAssertion->op1.lcl.lclNum == tree->gtLclVarCommon.GetLclNum() &&
                 tree->gtType == lvaTable[tree->gtLclVarCommon.GetLclNum()].lvType)
        {
            // If local assertion prop just, perform constant prop.
            if (optLocalAssertionProp)
            {
                return optConstantAssertionProp(curAssertion, tree, stmt DEBUGARG(index));
            }
            // If global assertion, perform constant propagation only if the VN's match and the lcl is non-CSE.
            else if (curAssertion->op1.vn == tree->gtVNPair.GetConservative())
            {
#if FEATURE_ANYCSE
                // Don't perform constant prop for CSE LclVars
                if (!lclNumIsCSE(tree->AsLclVarCommon()->GetLclNum()))
#endif
                {
                    return optConstantAssertionProp(curAssertion, tree, stmt DEBUGARG(index));
                }
            }
        }
    }
    return nullptr;
}


/*****************************************************************************
 *
 *  Given a set of "assertions" to search, find an assertion that matches
 *  op1Kind and lclNum, op2Kind and the constant value and is either equal or
 *  not equal assertion.
 */
unsigned Compiler::optLocalAssertionIsEqualOrNotEqual(optOp1Kind op1Kind, unsigned lclNum, optOp2Kind  op2Kind,
                                                      ssize_t cnsVal, EXPSET_TP assertions)
{
    noway_assert((op1Kind == O1K_LCLVAR) || (op1Kind == O1K_EXACT_TYPE) || (op1Kind == O1K_SUBTYPE));
    noway_assert((op2Kind == O2K_CONST_INT) || (op2Kind == O2K_IND_CNS_INT));
    if (!optLocalAssertionProp && assertions == 0)
    {
        return NO_ASSERTION_INDEX;
    }

    EXPSET_TP mask = 1;

    for (unsigned index = 1; index <= optAssertionCount; ++index, mask <<= 1)
    {
        noway_assert(mask == optGetAssertionBit(index));

        AssertionDsc* curAssertion = optGetAssertion(index);
        if  (optLocalAssertionProp || (assertions & mask))
        {
            if ((curAssertion->assertionKind != OAK_EQUAL) && (curAssertion->assertionKind != OAK_NOT_EQUAL))
            {
                continue;
            }

            if ((curAssertion->op1.kind == op1Kind) &&
                (curAssertion->op1.lcl.lclNum == lclNum) &&
                (curAssertion->op2.kind == op2Kind))
            {
                bool constantIsEqual  = (curAssertion->op2.u1.iconVal == cnsVal);
                bool assertionIsEqual = (curAssertion->assertionKind == OAK_EQUAL);                    

                if (constantIsEqual || assertionIsEqual)
                {
                    return index;
                }
            }
        }
    }
    return NO_ASSERTION_INDEX;
}

/*****************************************************************************
 *
 *  Given a set of "assertions" to search for, find an assertion that is either
 *  "op1" == "op2" or "op1" != "op2." Does a value number based comparison.
 *
 */
unsigned Compiler::optGlobalAssertionIsEqualOrNotEqual(EXPSET_TP assertions, GenTreePtr op1, GenTreePtr op2)
{
    if (assertions == 0)
    {
        return NO_ASSERTION_INDEX;
    }
    unsigned index = 1;
    for (EXPSET_TP mask = 1; index <= optAssertionCount; index++, mask <<= 1)
    {
        AssertionDsc* curAssertion = optGetAssertion(index);
        if ((curAssertion->assertionKind != OAK_EQUAL && curAssertion->assertionKind != OAK_NOT_EQUAL))
        {
            continue;
        }

        if (assertions & mask)
        {
            if (curAssertion->op1.vn == op1->gtVNPair.GetConservative() && curAssertion->op2.vn == op2->gtVNPair.GetConservative())
            {
                return index;
            }
        }
    }
    return NO_ASSERTION_INDEX;
}


/*****************************************************************************
 *
 *  Given a tree consisting of a RelOp and a set of available assertions
 *  we try to propagate an assertion and modify the RelOp tree if we can.
 *  We pass in the root of the tree via 'stmt', for local copy prop 'stmt' will be nullptr 
 *  Returns the modified tree, or nullptr if no assertion prop took place
 */

GenTreePtr Compiler::optAssertionProp_RelOp(EXPSET_TP assertions, const GenTreePtr tree, const GenTreePtr stmt)
{
    assert(tree->OperKind() & GTK_RELOP);

    //
    // Currently only GT_EQ or GT_NE are supported Relops for AssertionProp
    //
    if ((tree->gtOper != GT_EQ) && (tree->gtOper != GT_NE))
    {
        return nullptr;
    }

    if (!optLocalAssertionProp)
    {
        // If global assertion prop then use value numbering.
        return optAssertionPropGlobal_RelOp(assertions, tree, stmt);
    }
    else
    {
        // If local assertion prop then use variable based prop.
        return optAssertionPropLocal_RelOp(assertions, tree, stmt);
    }
}

/*************************************************************************************
 *
 *  Given the set of "assertions" to look up a relop assertion about the relop "tree",
 *  perform Value numbering based relop assertion propagation on the tree.
 *
 */
GenTreePtr Compiler::optAssertionPropGlobal_RelOp(EXPSET_TP assertions, const GenTreePtr tree, const GenTreePtr stmt)
{
    assert(tree->OperGet() == GT_EQ || tree->OperGet() == GT_NE);

    GenTreePtr newTree = tree;
    GenTreePtr op1 = tree->gtOp.gtOp1;
    GenTreePtr op2 = tree->gtOp.gtOp2;

    // If op2 is null and the VN for op1 is known to be non-null then transform the tree.
    if ((op2->gtVNPair.GetConservative() == ValueNumStore::VNForNull()) &&
        vnStore->IsKnownNonNull(op1->gtVNPair.GetConservative()))
    {
#ifdef DEBUG
        if (verbose)
        {
            printf("\nVN relop based non-null assertion prop for op1 in BB%02u:\n", compCurBB->bbNum);
            gtDispTree(op1, 0, nullptr, true);
        }
#endif
        op1->ChangeOperConst(GT_CNS_INT);
        op1->gtIntCon.gtIconVal = 0;
        op1->gtVNPair.SetBoth(ValueNumStore::VNForNull());   

        gtReverseCond(tree);

        newTree = fgMorphTree(tree);

#ifdef DEBUG
        if (verbose)
        {
            gtDispTree(newTree, 0, nullptr, true);
        }
#endif
        return optAssertionProp_Update(newTree, tree, stmt);
    }

    // If op1 is null and the VN for op2 is known to be non-null then transform the tree.
    if ((op1->gtVNPair.GetConservative() == ValueNumStore::VNForNull()) &&
        vnStore->IsKnownNonNull(op2->gtVNPair.GetConservative()))
    {
#ifdef DEBUG
        if (verbose)
        {
            printf("\nVN relop based non-null assertion prop for op2 in BB%02u:\n", compCurBB->bbNum);
            gtDispTree(op2, 0, nullptr, true);
        }
#endif
        op2->ChangeOperConst(GT_CNS_INT);
        op2->gtIntCon.gtIconVal = 0;
        op1->gtVNPair.SetBoth(ValueNumStore::VNForNull());   

        gtReverseCond(tree);

        newTree = fgMorphTree(tree);

#ifdef DEBUG
        if (verbose)
        {
            gtDispTree(newTree, 0, nullptr, true);
        }
#endif
        return optAssertionProp_Update(newTree, tree, stmt);
    }

    if (op1->gtOper != GT_LCL_VAR)
    {
        return nullptr;
    }

    // Find an equal or not equal assertion involving "op1" and "op2".
    unsigned index = optGlobalAssertionIsEqualOrNotEqual(assertions, op1, op2);
    if (index == NO_ASSERTION_INDEX)
    {
        return nullptr;
    }

    AssertionDsc* curAssertion = optGetAssertion(index);

    // Allow or not to reverse condition for OAK_NOT_EQUAL assertions.
    bool allowReverse = true;

    // If the assertion involves "op2" and it is a constant, then check if "op1" also has a constant value.
    if (vnStore->IsVNConstant(op2->gtVNPair.GetConservative()))
    {
        ValueNum   vnCns = op2->gtVNPair.GetConservative();
#ifdef DEBUG
        if  (verbose)
        {
            printf("\nVN relop based constant assertion prop in BB%02u:\n", compCurBB->bbNum);
            printf("Assertion index=#%02u: ", index);
            printTreeID(op1);
            printf(" %s ", (curAssertion->assertionKind == OAK_EQUAL) ? "==" : "!=");
            if (genActualType(op1->TypeGet()) == TYP_INT)
            {
                printf("%d\n", vnStore->ConstantValue<int>(vnCns));
            }
            else if (op1->TypeGet() == TYP_LONG)
            {
                printf("%I64d\n", vnStore->ConstantValue<INT64>(vnCns));
            }
            else if (op1->TypeGet() == TYP_DOUBLE)
            {
                printf("%f\n", vnStore->ConstantValue<double>(vnCns));
            }
            else if (op1->TypeGet() == TYP_FLOAT)
            {
                printf("%f\n", vnStore->ConstantValue<float>(vnCns));
            }
            else if (op1->TypeGet() == TYP_REF)
            {
                // The only constant of TYP_REF that ValueNumbering supports is 'null'
                assert(vnStore->ConstantValue<size_t>(vnCns) == 0);
                printf("null\n");
            }
            else
            {
                printf("??unknown\n");
            }
            gtDispTree(tree, 0, nullptr, true);
        }
#endif
        // Decrement the ref counts, before we change the oper.
        lvaTable[op1->gtLclVar.gtLclNum].decRefCnts(compCurBB->getBBWeight(this), this);

        // Change the oper to const.
        if (genActualType(op1->TypeGet()) == TYP_INT)
        {
            op1->ChangeOperConst(GT_CNS_INT);
            op1->gtIntCon.gtIconVal = vnStore->ConstantValue<int>(vnCns);
        }
        else if (op1->TypeGet() == TYP_LONG)
        {
            op1->ChangeOperConst(GT_CNS_NATIVELONG);
            op1->gtIntConCommon.SetLngValue(vnStore->ConstantValue<INT64>(vnCns));
        }
        else if (op1->TypeGet() == TYP_DOUBLE)
        {
            double constant = vnStore->ConstantValue<double>(vnCns);
            op1->ChangeOperConst(GT_CNS_DBL);
            op1->gtDblCon.gtDconVal = constant;

            // Nothing can be equal to NaN. So if IL had "op1 == NaN", then we already made op1 NaN,
            // which will yield a false correctly. Instead if IL had "op1 != NaN", then we already
            // made op1 NaN which will yield a true correctly. Note that this is irrespective of the
            // assertion we have made.
            allowReverse = (_isnan(constant) == 0);
        }
        else if (op1->TypeGet() == TYP_FLOAT)
        {
            float constant = vnStore->ConstantValue<float>(vnCns);
            op1->ChangeOperConst(GT_CNS_DBL);
            op1->gtDblCon.gtDconVal = constant;
            // See comments for TYP_DOUBLE.
            allowReverse = (_isnan(constant) == 0);
        }
        else if (op1->TypeGet() == TYP_REF)
        {
            op1->ChangeOperConst(GT_CNS_INT);
            // The only constant of TYP_REF that ValueNumbering supports is 'null'
            noway_assert(vnStore->ConstantValue<size_t>(vnCns) == 0);
            op1->gtIntCon.gtIconVal = 0;
        }
        else 
        {
            noway_assert(!"unknown type in Global_RelOp");
        }

        op1->gtVNPair.SetBoth(vnCns);      // Preserve the ValueNumPair, as ChangeOperConst/SetOper will clear it.
    }
    // If the assertion involves "op2" and "op1" is also a local var, then just morph the tree.
    else if (op2->gtOper == GT_LCL_VAR)
    {
#ifdef DEBUG
        if  (verbose)
        {
            printf("\nVN relop based copy assertion prop in BB%02u:\n", compCurBB->bbNum);
            printf("Assertion index=#%02u: V%02d.%02d %s V%02d.%02d\n", index,
                   op1->gtLclVar.gtLclNum, op1->gtLclVar.gtSsaNum, 
                   (curAssertion->assertionKind == OAK_EQUAL) ? "==" : "!=",
                   op2->gtLclVar.gtLclNum, op2->gtLclVar.gtSsaNum);
            gtDispTree(tree, 0, nullptr, true);
        }
#endif
        lvaTable[op1->gtLclVar.gtLclNum].decRefCnts(compCurBB->getBBWeight(this), this);

        // If floating point, don't just substitute op1 with op2, this won't work if
        // op2 is NaN. Just turn it into a "true" or "false" yielding expression.
        if (op1->TypeGet() == TYP_DOUBLE || op1->TypeGet() == TYP_FLOAT)
        {
            // Note we can't trust the OAK_EQUAL as the value could end up being a NaN
            // violating the assertion. However, we create OAK_EQUAL assertions for floating
            // point only on JTrue nodes, so if the condition held earlier, it will hold 
            // now. We don't create OAK_EQUAL assertion on floating point from GT_ASG
            // because we depend on value num which would constant prop the NaN.
            lvaTable[op2->gtLclVar.gtLclNum].decRefCnts(compCurBB->getBBWeight(this), this);
            op1->ChangeOperConst(GT_CNS_DBL);
            op1->gtDblCon.gtDconVal = 0;
            op2->ChangeOperConst(GT_CNS_DBL);
            op2->gtDblCon.gtDconVal = 0;
        }
        // Change the op1 LclVar to the op2 LclVar
        else
        {
            noway_assert(varTypeIsIntegralOrI(op1->TypeGet()));
            lvaTable[op2->gtLclVar.gtLclNum].incRefCnts(compCurBB->getBBWeight(this), this);
            op1->AsLclVarCommon()->SetLclNum(op2->AsLclVarCommon()->GetLclNum());
            op1->AsLclVarCommon()->SetSsaNum(op2->AsLclVarCommon()->GetSsaNum());
        }
    }
    else
    {
        return nullptr;
    }

    // Finally reverse the condition, if we have a not equal assertion.
    if (allowReverse && curAssertion->assertionKind == OAK_NOT_EQUAL)
    {
        gtReverseCond(tree);
    }

    newTree = fgMorphTree(tree);

#ifdef DEBUG
    if (verbose)
    {
        gtDispTree(newTree, 0, nullptr, true);
    }
#endif

    return optAssertionProp_Update(newTree, tree, stmt);
}

/*************************************************************************************
 *
 *  Given the set of "assertions" to look up a relop assertion about the relop "tree",
 *  perform local variable name based relop assertion propagation on the tree.
 *
 */
GenTreePtr Compiler::optAssertionPropLocal_RelOp(EXPSET_TP assertions, const GenTreePtr tree, const GenTreePtr stmt)
{
    assert(tree->OperGet() == GT_EQ || tree->OperGet() == GT_NE);

    GenTreePtr op1 = tree->gtOp.gtOp1;
    GenTreePtr op2 = tree->gtOp.gtOp2;

    // For Local AssertionProp we only can fold when op1 is a GT_LCL_VAR
    if (op1->gtOper != GT_LCL_VAR)
    {
        return nullptr;
    }

    // For Local AssertionProp we only can fold when op2 is a GT_CNS_INT
    if (op2->gtOper != GT_CNS_INT)
    {
        return nullptr;
    }

    optOp1Kind  op1Kind = O1K_LCLVAR;
    optOp2Kind  op2Kind = O2K_CONST_INT;
    ssize_t     cnsVal  = op2->gtIntCon.gtIconVal;
    var_types   cmpType = op1->TypeGet();

    // Don't try to fold/optimize Floating Compares; there are multiple zero values. 
    if (varTypeIsFloating(cmpType))
        return nullptr;

    // Find an equal or not equal assertion about op1 var.
    unsigned  lclNum = op1->gtLclVarCommon.gtLclNum; noway_assert(lclNum < lvaCount);
    unsigned  index  = optLocalAssertionIsEqualOrNotEqual(op1Kind, lclNum, op2Kind, cnsVal, assertions);

    if (index == NO_ASSERTION_INDEX)
    {
        return nullptr;
    }

    AssertionDsc* curAssertion = optGetAssertion(index);

    bool assertionKindIsEqual = (curAssertion->assertionKind == OAK_EQUAL);                    
    bool constantIsEqual = false;

    if (genTypeSize(cmpType) == TARGET_POINTER_SIZE)
    {
        constantIsEqual = (curAssertion->op2.u1.iconVal == cnsVal);
    }
#ifdef _TARGET_64BIT_
    else if (genTypeSize(cmpType) == sizeof(INT32))
    {
        // Compare the low 32-bits only
        constantIsEqual = (((INT32)curAssertion->op2.u1.iconVal) == ((INT32)cnsVal));
    }
#endif
    else
    {
        // We currently don't fold/optimze when the GT_LCL_VAR has been cast to a small type
        return nullptr;
    }

    noway_assert(constantIsEqual || assertionKindIsEqual);

#ifdef  DEBUG
    if  (verbose)
    {
        printf("\nAssertion prop for index #%02u in BB%02u:\n",
                index, compCurBB->bbNum); 
        gtDispTree(tree, 0, nullptr, true);
    }                
#endif

    // Return either CNS_INT 0 or CNS_INT 1.
    bool foldResult = (constantIsEqual == assertionKindIsEqual);
    if (tree->gtOper == GT_NE)
       foldResult = !foldResult;

    op2->gtIntCon.gtIconVal = foldResult;
    op2->gtType = TYP_INT;

    return optAssertionProp_Update(op2, tree, stmt);
}

/*****************************************************************************
 *
 *  Given a tree consisting of a Cast and a set of available assertions
 *  we try to propagate an assertion and modify the Cast tree if we can.
 *  We pass in the root of the tree via 'stmt', for local copy prop 'stmt'
 *  will be nullptr.
 *
 *  Returns the modified tree, or nullptr if no assertion prop took place.
 */
GenTreePtr Compiler::optAssertionProp_Cast(EXPSET_TP assertions, const GenTreePtr tree, const GenTreePtr stmt)
{
    assert(tree->gtOper == GT_CAST);

    var_types  toType = tree->gtCast.gtCastType;
    GenTreePtr op1    = tree->gtCast.CastOp();

    // If we have a cast involving floating point types, then bail.
    if (varTypeIsFloating(toType) || varTypeIsFloating(op1->TypeGet()))
    {
        return nullptr;
    }

    // Skip over a GT_COMMA node(s), if necessary to get to the lcl.
    GenTreePtr lcl = op1;
    while (lcl->gtOper == GT_COMMA)
    {
        lcl = lcl->gtOp.gtOp2;
    }

    // If we don't have a cast of a LCL_VAR then bail.
    if (lcl->gtOper != GT_LCL_VAR)
    {
        return nullptr;
    }

    unsigned index = optAssertionIsSubrange(lcl, toType, assertions);
    if (index != NO_ASSERTION_INDEX)
    {
        LclVarDsc* varDsc = &lvaTable[lcl->gtLclVarCommon.gtLclNum];
        if (varDsc->lvNormalizeOnLoad() || varTypeIsLong(varDsc->TypeGet()))
        {
            // For normalize on load variables it must be a narrowing cast to remove
            if (genTypeSize(toType) > genTypeSize(varDsc->TypeGet()))
            {   
                // Can we just remove the GTF_OVERFLOW flag?
                if ((tree->gtFlags & GTF_OVERFLOW) == 0)
                {
                    return nullptr;
                }
                else
                {

#ifdef  DEBUG
                    if  (verbose)
                    {
                        printf("\nSubrange prop for index #%02u in BB%02u:\n", index, compCurBB->bbNum); 
                        gtDispTree(tree, 0, nullptr, true);
                    }
#endif
                    tree->gtFlags &= ~GTF_OVERFLOW; // This cast cannot overflow 
                    return optAssertionProp_Update(tree, tree, stmt);
                }
            }

            //             GT_CAST   long -> uint -> int
            //                |
            //           GT_LCL_VAR long
            //
            // Where the lclvar is known to be in the range of [0..MAX_UINT]
            //
            // A load of a 32-bit unsigned int is the same as a load of a 32-bit signed int 
            // 
            if (toType == TYP_UINT)
            {
                toType = TYP_INT;
            }

            // Change the "lcl" type to match what the cast wanted, by propagating the type
            // change down the comma nodes leading to the "lcl", if we skipped them earlier.
            GenTreePtr tmp = op1;
            while (tmp->gtOper == GT_COMMA)
            {
                tmp->gtType = toType;
                tmp = tmp->gtOp.gtOp2;
            }
            noway_assert(tmp == lcl);
            tmp->gtType = toType;
        }

#ifdef  DEBUG
        if  (verbose)
        {
            printf("\nSubrange prop for index #%02u in BB%02u:\n", 
                   index, compCurBB->bbNum); 
            gtDispTree(tree, 0, nullptr, true);
        }
#endif
        return optAssertionProp_Update(op1, tree, stmt);
    }
    return nullptr;
}

/*****************************************************************************
 *
 *  Given a tree with an array bounds check node, eliminate it because it was
 *  checked already in the program.
 */
GenTreePtr          Compiler::optAssertionProp_Comma(EXPSET_TP assertions,
                                                     const GenTreePtr tree,
                                                     const GenTreePtr stmt)
{
    // Process the bounds check as part of the GT_COMMA node since, we need parent pointer to remove nodes.
    if (tree->gtGetOp1()->OperGet() == GT_ARR_BOUNDS_CHECK)
    {
        // Since the GT_COMMA tree gets processed by assertion prop after the child GT_ARR_BOUNDS_CHECK
        // node in execution order, bounds check assertions will be included for the parent GT_COMMA node.
        // Remove the assertion made by the bounds check tree about itself.
        unsigned index = tree->gtGetOp1()->GetAssertion();
        EXPSET_TP newAssertions = (index != NO_ASSERTION_INDEX && optGetAssertion(index)->IsBoundsCheckNoThrow())
                                ? assertions & ~optGetAssertionBit(index)
                                : assertions;

        return optAssertionProp_BndsChk(newAssertions, tree, stmt);
    }
    return nullptr;
}

/*****************************************************************************
 *
 *  Given a tree consisting of a Ind and a set of available assertions, we try
 *  to propagate an assertion and modify the Ind tree if we can. We pass in the
 *  root of the tree via 'stmt', for local copy prop 'stmt' will be nullptr.
 *
 *  Returns the modified tree, or nullptr if no assertion prop took place.
 *
 */

GenTreePtr Compiler::optAssertionProp_Ind(EXPSET_TP  assertions, const GenTreePtr tree, const GenTreePtr stmt)
{
    assert(tree->OperIsIndir());

    if (!(tree->gtFlags & GTF_EXCEPT))
    {
        return nullptr;
    }

    // Check for add of a constant.
    GenTreePtr  op1 = tree->gtOp.gtOp1;
    if ((op1->gtOper == GT_ADD) &&  (op1->gtOp.gtOp2->gtOper == GT_CNS_INT))
    {
        op1 = op1->gtOp.gtOp1;  
    }

    if (op1->gtOper != GT_LCL_VAR)
    {
        return nullptr;
    }

    unsigned lclNum = op1->gtLclVarCommon.gtLclNum;

#ifdef DEBUG
    bool vnBased = false;
    unsigned index = NO_ASSERTION_INDEX;
#endif
    if (optAssertionIsNonNull(op1, assertions DEBUGARG(&vnBased) DEBUGARG(&index)))
    {
#ifdef  DEBUG
        if  (verbose)
        {
            (vnBased) ? printf("\nVN based non-null prop in BB%02u:\n", compCurBB->bbNum)
                      : printf("\nNon-null prop for index #%02u in BB%02u:\n", index, compCurBB->bbNum);
            gtDispTree(tree, 0, nullptr, true);
        }
#endif
        tree->gtFlags &= ~GTF_EXCEPT;

        // Set this flag to prevent reordering
        tree->gtFlags |=  GTF_ORDER_SIDEEFF;
        
        return optAssertionProp_Update(tree, tree, stmt);
    }

    return nullptr;
}

/*****************************************************************************
 *  Check if a non-null assertion can be made about the input operand "op"
 *  from the set of "assertions," or implicitly from the value number on "op."
 *
 *  Sets "pVnBased" if the assertion is value number based. If no matching
 *  assertions are found from the table, then returns "NO_ASSERTION_INDEX."
 *
 *  Note: If both VN and assertion table yield a matching assertion, "pVnBased"
 *  is only set and the return value is "NO_ASSERTION_INDEX."
 */
bool Compiler::optAssertionIsNonNull(GenTreePtr op, EXPSET_TP assertions DEBUGARG(bool* pVnBased) DEBUGARG(unsigned* pIndex))
{
    bool vnBased = (!optLocalAssertionProp && vnStore->IsKnownNonNull(op->gtVNPair.GetConservative()));
#ifdef DEBUG
    *pVnBased = vnBased;
#endif

    if (vnBased)
    {
#ifdef DEBUG
        *pIndex = NO_ASSERTION_INDEX;
#endif
        return true;
    }

    unsigned index = optAssertionIsNonNullInternal(op, assertions);
#ifdef DEBUG
    *pIndex = index;
#endif
    return index != NO_ASSERTION_INDEX;
}

/*****************************************************************************
 *  Check if a non-null assertion can be made about the input operand "op"
 *  from the set of "assertions."
 *
 */
unsigned Compiler::optAssertionIsNonNullInternal(GenTreePtr op, EXPSET_TP assertions)
{
    // If local assertion prop use lcl comparison, else use VN comparison.
    if (!optLocalAssertionProp)
    {
        ValueNum vn = op->gtVNPair.GetConservative();

        if (assertions == 0)
        {
            return NO_ASSERTION_INDEX;
        }

        // Check each assertion to find if we have a vn == or != null assertion.
        unsigned index = 1;
        for (EXPSET_TP mask = 1; index <= optAssertionCount; index++, mask <<= 1)
        {
            if (assertions & mask)
            {
                AssertionDsc* curAssertion = optGetAssertion(index);
                if (curAssertion->assertionKind != OAK_NOT_EQUAL)
                {
                    continue;
                }
                if (curAssertion->op1.vn != vn || curAssertion->op2.vn != ValueNumStore::VNForNull())
                {
                    continue;
                }
                return index;
            }
        }
    }
    else
    {
        unsigned lclNum = op->AsLclVarCommon()->GetLclNum();
        // Check each assertion to find if we have a variable == or != null assertion.
        unsigned index = 1;
        for (EXPSET_TP mask = 1; index <= optAssertionCount; index++, mask <<= 1)
        {
            AssertionDsc* curAssertion = optGetAssertion(index);
            if  ((curAssertion->assertionKind == OAK_NOT_EQUAL) && // kind
                 (curAssertion->op1.kind == O1K_LCLVAR) && // op1
                 (curAssertion->op2.kind == O2K_CONST_INT) && // op2
                 (curAssertion->op1.lcl.lclNum == lclNum) &&
                 (curAssertion->op2.u1.iconVal == 0))
            {
                return index;
            }
        }
    }
    return NO_ASSERTION_INDEX;
}

/*****************************************************************************
 *
 *  Given a tree consisting of a call and a set of available assertions, we
 *  try to propagate an assertion and modify the Call tree if we can. Our
 *  current modifications are limited to removing the nullptrCHECK flag from
 *  the call.
 *  We pass in the root of the tree via 'stmt', for local copy prop 'stmt'
 *  will be nullptr. Returns the modified tree, or nullptr if no assertion prop
 *  took place.
 *
 */

GenTreePtr Compiler::optAssertionProp_Call(EXPSET_TP assertions, const GenTreePtr tree, const GenTreePtr stmt)
{
    assert(tree->gtOper == GT_CALL);

    if ((tree->gtFlags & GTF_CALL_NULLCHECK) != 0)
    {
        GenTreePtr op1 = gtGetThisArg(tree);
        noway_assert(op1 != nullptr);
        if (op1->gtOper != GT_LCL_VAR)
        {
            return nullptr;
        }

#ifdef DEBUG
        bool vnBased = false;
        unsigned index = NO_ASSERTION_INDEX;
#endif
        if (optAssertionIsNonNull(op1, assertions DEBUGARG(&vnBased) DEBUGARG(&index)))
        {
#ifdef  DEBUG
            if  (verbose)
            {
                (vnBased) ? printf("\nVN based non-null prop in BB%02u:\n", compCurBB->bbNum)
                          : printf("\nNon-null prop for index #%02u in BB%02u:\n", index, compCurBB->bbNum);
                gtDispTree(tree, 0, nullptr, true);
            }
#endif
            tree->gtFlags &= ~GTF_CALL_NULLCHECK;
            tree->gtFlags &= ~GTF_EXCEPT;
            noway_assert(tree->gtFlags & GTF_SIDE_EFFECT);
            
            return optAssertionProp_Update(tree, tree, stmt);
        }
    }
    else if (!optLocalAssertionProp && (tree->gtCall.gtCallType == CT_HELPER))
    {
        if (tree->gtCall.gtCallMethHnd == eeFindHelper(CORINFO_HELP_ISINSTANCEOFINTERFACE) ||
            tree->gtCall.gtCallMethHnd == eeFindHelper(CORINFO_HELP_ISINSTANCEOFARRAY) ||
            tree->gtCall.gtCallMethHnd == eeFindHelper(CORINFO_HELP_ISINSTANCEOFCLASS) ||
            tree->gtCall.gtCallMethHnd == eeFindHelper(CORINFO_HELP_ISINSTANCEOFANY) ||
            tree->gtCall.gtCallMethHnd == eeFindHelper(CORINFO_HELP_CHKCASTINTERFACE) ||
            tree->gtCall.gtCallMethHnd == eeFindHelper(CORINFO_HELP_CHKCASTARRAY) ||
            tree->gtCall.gtCallMethHnd == eeFindHelper(CORINFO_HELP_CHKCASTCLASS) ||
            tree->gtCall.gtCallMethHnd == eeFindHelper(CORINFO_HELP_CHKCASTANY) ||
            tree->gtCall.gtCallMethHnd == eeFindHelper(CORINFO_HELP_CHKCASTCLASS_SPECIAL))
        {
            GenTreePtr arg1 = gtArgEntryByArgNum(tree->AsCall(), 1)->node;
            if (arg1->gtOper != GT_LCL_VAR)
            {
                return nullptr;
            }

            GenTreePtr arg2 = gtArgEntryByArgNum(tree->AsCall(), 0)->node;

            unsigned index = optAssertionIsSubtype(arg1, arg2, assertions);
            if (index != NO_ASSERTION_INDEX)
            {
#ifdef  DEBUG
                if  (verbose)
                {
                    printf("\nDid VN based subtype prop for index #%02u in BB%02u:\n",
                           index, compCurBB->bbNum); 
                    gtDispTree(tree, 0, nullptr, true);
                }
#endif
                GenTreePtr list = nullptr;
                gtExtractSideEffList(tree, &list, GTF_SIDE_EFFECT, true);
                if (list != nullptr)
                {
                    arg1 = gtNewOperNode(GT_COMMA, tree->TypeGet(), list, arg1);
                    fgSetTreeSeq(arg1);
                }

                return optAssertionProp_Update(arg1, tree, stmt);
            }
        }            
    }


    return nullptr;
}

/*****************************************************************************
 *
 *  Given a tree consisting of a comma node with a bounds check, remove any
 *  redundant bounds check that has already been checked in the program flow.
 */
GenTreePtr Compiler::optAssertionProp_BndsChk(EXPSET_TP assertions, const GenTreePtr tree, const GenTreePtr stmt)
{
    if (optLocalAssertionProp)
    {
        return nullptr;
    }

    assert(tree->gtOper == GT_COMMA && tree->gtGetOp1()->OperGet() == GT_ARR_BOUNDS_CHECK);

    unsigned index = 1;
    for (EXPSET_TP mask = 1; index <= optAssertionCount; index++, mask <<= 1)
    {
        if ((assertions & mask) == 0)
        {
            continue;
        }

        // If it is not a nothrow assertion, skip.
        AssertionDsc* curAssertion = optGetAssertion(index);
        if (!curAssertion->IsBoundsCheckNoThrow())
        {
            continue;
        }

        GenTreeBoundsChk* arrBndsChk = tree->gtGetOp1()->AsBoundsChk();

        // Set 'isRedundant' to true if we can determine that 'arrBndsChk' can be 
        // classified as a redundant bounds check using 'curAssertion'
        bool isRedundant = false;
#ifdef  DEBUG
        const char* dbgMsg = "Not Set";
#endif

        // Do we have a previous range check involving the same 'vnLen' upper bound?
        if (curAssertion->op1.bnd.vnLen == arrBndsChk->gtArrLen->gtVNPair.GetConservative())
        {
            ValueNum vnCurIdx = arrBndsChk->gtIndex->gtVNPair.GetConservative();

            // Do we have the exact same lower bound 'vnIdx'?
            //       a[i] followed by a[i]
            if (curAssertion->op1.bnd.vnIdx == vnCurIdx)
            {
                isRedundant = true;
#ifdef  DEBUG
                dbgMsg = "a[i] followed by a[i]";
#endif
            }
            // Are we using zero as the index? 
            // It can always be considered as redundant with any previous value
            //       a[*] followed by a[0]
            else if (vnCurIdx == vnStore->VNZeroForType(arrBndsChk->gtIndex->TypeGet()))
            {
                isRedundant = true;
#ifdef  DEBUG
                dbgMsg = "a[*] followed by a[0]";
#endif
            }
            // Do we have two constant indexes?
            else if (vnStore->IsVNConstant(curAssertion->op1.bnd.vnIdx) && vnStore->IsVNConstant(vnCurIdx))
            {
                // Make sure the types match.
                var_types type1 = vnStore->TypeOfVN(curAssertion->op1.bnd.vnIdx);
                var_types type2 = vnStore->TypeOfVN(vnCurIdx);

                if (type1 == type2 && type1 == TYP_INT)
                {
                    int index1 = vnStore->ConstantValue<int>(curAssertion->op1.bnd.vnIdx);
                    int index2 = vnStore->ConstantValue<int>(vnCurIdx);

                    // the case where index1 == index2 should have been handled above
                    assert(index1 != index2);

                    // It can always be considered as redundant with any previous higher constant value
                    //       a[K1] followed by a[K2], with K2 >= 0 and K1 >= K2
                    if (index2 >= 0 && index1 >= index2)
                    {
                        isRedundant = true;
#ifdef  DEBUG
                        dbgMsg = "a[K1] followed by a[K2], with K2 >= 0 and K1 >= K2";
#endif
                    }
                }
            }
            // Extend this to remove additional redundant bounds checks:
            // i.e.  a[i+1] followed by a[i]  by using the VN(i+1) >= VN(i)
            //       a[i]   followed by a[j]  when j is known to be >= i
            //       a[i]   followed by a[5]  when i is known to be >= 5
        }

        if (!isRedundant)
            continue;

#ifdef  DEBUG
        if  (verbose)
        {
            printf("\nVN based redundant (%s) bounds check assertion prop for index #%02u in BB%02u:\n",
                    dbgMsg, index, compCurBB->bbNum);
            gtDispTree(tree, 0, nullptr, true);
        }
#endif
        optRemoveRangeCheck(tree, stmt, true, GTF_ASG, true /* force remove */);
        GenTreePtr newTree = optAssertionProp_Update(tree, tree, stmt);
        if (newTree != nullptr)
        {
            return optAssertionProp(assertions & ~mask, tree, stmt);
        }
    }
    return nullptr;
}

/*****************************************************************************
 *
 *  Called when we have a successfully performed an assertion prop. We have
 *  the newTree in hand. This method will replace the existing tree in the
 *  stmt with the newTree.
 *
 */

GenTreePtr Compiler::optAssertionProp_Update(const GenTreePtr newTree, const GenTreePtr tree, const GenTreePtr stmt)
{
    noway_assert(newTree != nullptr);

    if (stmt == nullptr)
    {
        noway_assert(optLocalAssertionProp);
    }
    else
    {
        noway_assert(!optLocalAssertionProp);

        // If newTree == tree then we modified the tree in-place otherwise we have to
        // locate our parent node and update it so that it points to newTree
        if (newTree != tree)
        {
            GenTreePtr * link = gtFindLink(stmt, tree);
#ifdef DEBUG
            if (link == nullptr)
            {
                noway_assert(!"gtFindLink failed!");
                printf("\nCould not find parent of:\n");
                gtDispTree(tree);
                printf("\nIn this stmt:\n");
                gtDispTree(stmt);
            }
#endif
            noway_assert(link != nullptr);
            noway_assert(tree != nullptr);
            if (link != nullptr)
            {
                // Replace the old operand with the newTree
                *link = newTree;

                // We only need to ensure that the gtNext field is set as it is used to traverse
                // to the next node in the tree. We will re-morph this entire statement in
                // optAssertionPropMain(). It will reset the gtPrev and gtNext links for all nodes.

                newTree->gtNext = tree->gtNext;
            }
        }
    }

    // Record that we propagated the assertion.
    optAssertionPropagated = true;
    optAssertionPropagatedCurrentStmt = true;

    return newTree;
}


/*****************************************************************************
 *
 *  Given a tree and a set of available assertions we try to propagate an
 *  assertion and modify 'tree' if we can. We pass in the root of the tree
 *  via 'stmt', for local copy prop 'stmt' will be nullptr.
 *
 *  Returns the modified tree, or nullptr if no assertion prop took place.
 */

GenTreePtr          Compiler::optAssertionProp(EXPSET_TP  assertions, 
                                               const GenTreePtr tree,
                                               const GenTreePtr stmt)
{
    switch (tree->gtOper)
    {
    case GT_LCL_VAR:
        return optAssertionProp_LclVar(assertions, tree, stmt);

    case GT_IND:
    case GT_NULLCHECK:
        return optAssertionProp_Ind(assertions, tree, stmt);

    case GT_COMMA:
        return optAssertionProp_Comma(assertions, tree, stmt);

    case GT_CAST:
        return optAssertionProp_Cast(assertions, tree, stmt);

    case GT_CALL:
        return optAssertionProp_Call(assertions, tree, stmt);

    case GT_EQ:
    case GT_NE:
    case GT_LT:
    case GT_LE:
    case GT_GT:
    case GT_GE:

        return optAssertionProp_RelOp(assertions, tree, stmt);

    default:
        return nullptr;
    } 
}

/*****************************************************************************
 *
 *   Given a tree node that makes an assertion
 *   this method computes the set of implied assertions that are also true
 */

EXPSET_TP Compiler::optImpliedAssertions(unsigned assertionIndex, EXPSET_TP activeAssertions)
{
    noway_assert(!optLocalAssertionProp);
    noway_assert(assertionIndex != 0);
    noway_assert(assertionIndex <= optAssertionCount);

    AssertionDsc* curAssertion = this->optGetAssertion(assertionIndex);
    if (activeAssertions)
    {
        EXPSET_TP chkAssertionMask = optGetVnMappedAssertions(curAssertion->op1.vn);
        if (curAssertion->op2.kind == O2K_LCLVAR_COPY)
        {
            chkAssertionMask |= optGetVnMappedAssertions(curAssertion->op2.vn);
        }
        chkAssertionMask &= activeAssertions;

        if (!chkAssertionMask)
        {
            return 0;
        }

        bool curIsCopyAssertion = curAssertion->IsCopyAssertion();
        EXPSET_TP result = 0;

        // Check each assertion in chkAssertionMask to see if it can be applied to curAssertion
        EXPSET_TP singleBit = 1;
        for (unsigned chkIndex = 1; chkIndex <= optAssertionCount; chkIndex++, singleBit <<= 1)
        {
            // If assertions to check becomes empty skip.
            if (!chkAssertionMask)
            {
                break;
            }
            
            // Is iterated assertion in assertions to be checked?
            if (!(singleBit & chkAssertionMask))
            {
                continue;
            }

            // Clear the iterated assertion from assertions to be checked.
            chkAssertionMask &= ~singleBit;

            if (chkIndex == assertionIndex)
            {
                continue;
            }

            // Determine which one is a copy assertion and use the other to check for implied assertions.
            AssertionDsc* iterAssertion = optGetAssertion(chkIndex);
            if (curIsCopyAssertion)
            {
                result |= optImpliedByCopyAssertion(curAssertion, iterAssertion);
            }
            else if (iterAssertion->IsCopyAssertion())
            {
                result |= optImpliedByCopyAssertion(iterAssertion, curAssertion);
            }
        }
        return result;
    }

    // Is curAssertion a constant assignment of a 32-bit integer?
    // (i.e  GT_LVL_VAR X  == GT_CNS_INT)
    if ((curAssertion->assertionKind == OAK_EQUAL) && 
        (curAssertion->op1.kind == O1K_LCLVAR) &&
        (curAssertion->op2.kind == O2K_CONST_INT))
    {
        return optImpliedByConstAssertion(curAssertion);
    }

    return NO_ASSERTION_INDEX;
}

/*****************************************************************************
 *
 *   Given a set of active assertions this method computes the set 
 *   of non-Null implied assertions that are also true
 */

EXPSET_TP Compiler::optImpliedByTypeOfAssertions(EXPSET_TP activeAssertions)
{
    EXPSET_TP result = 0;

    if (activeAssertions == 0)
    {
        return result;
    }

    // Check each assertion in activeAssertions to see if it can be applied to constAssertion
    EXPSET_TP singleBit = 1;

    for (unsigned chkIndex = 1; chkIndex <= optAssertionCount; chkIndex++, singleBit <<= 1)
    {
        if (!activeAssertions)
        {
            break;
        }

        assert(singleBit == optGetAssertionBit(chkIndex));

        // If assertion not currently in active set, skip.
        if (!(singleBit & activeAssertions))
        {
            continue;
        }

        // Remove the singleBit from the chkAssertionMask
        activeAssertions &= ~singleBit;

        // chkAssertion must be Type/Subtype is equal assertion
        AssertionDsc* chkAssertion = optGetAssertion(chkIndex);
        if ((chkAssertion->op1.kind != O1K_SUBTYPE && chkAssertion->op1.kind != O1K_EXACT_TYPE) ||
            (chkAssertion->assertionKind != OAK_EQUAL))
        {
            continue;
        }

        // Search the assertion table for a non-null assertion on op1 that matches chkAssertion
        for (unsigned impIndex = 1; impIndex <= optAssertionCount; impIndex++)
        {
            AssertionDsc* impAssertion = optGetAssertion(impIndex);

            //  The impAssertion must be different from the chkAssertion
            if (impIndex == chkIndex)
            {
                continue;
            }

            // impAssertion must be a Non Null assertion on lclNum
            if (impAssertion->assertionKind != OAK_NOT_EQUAL ||
                impAssertion->op1.kind != O1K_LCLVAR ||
                impAssertion->op2.kind != O2K_CONST_INT ||
                impAssertion->op1.vn != chkAssertion->op1.vn)
            {
                continue;
            }

            EXPSET_TP impAssertBit = optGetAssertionBit(impIndex);

            // The bit may already be in the result set
            if ((result & impAssertBit) == 0)
            {
                result |= optGetAssertionBit(impIndex);
#ifdef  DEBUG
                if  (verbose)
                {
                    printf("\nCompiler::optImpliedByTypeOfAssertions: %s Assertion #%02d, implies assertion #%02d",
                            (chkAssertion->op1.kind == O1K_SUBTYPE) ? "Subtype" : "Exact-type", chkIndex, impIndex);
                }
#endif
            }

            // There is at most one non-null assertion that is implied by the current chkIndex assertion
            break;
        }
    }

    // Note 'result' will either be zero if no new assertions are implied
    // or have exactly one bit set, representing the new implied assertion
    return result;
}

/*****************************************************************************
 *  Given a value number "vn", get the assertions we have about "vn."
 */

EXPSET_TP Compiler::optGetVnMappedAssertions(ValueNum vn)
{
    EXPSET_TP set = 0;
    if (optValueNumToAsserts->Lookup(vn, &set))
    {
        return set;
    }
    return 0;
}

/*****************************************************************************
 *
 *   Given a const assertion this method computes the set of implied assertions 
 *   that are also true
 */

EXPSET_TP Compiler::optImpliedByConstAssertion(AssertionDsc* constAssertion)
{  
    noway_assert(constAssertion->assertionKind == OAK_EQUAL);
    noway_assert(constAssertion->op1.kind == O1K_LCLVAR);    
    noway_assert(constAssertion->op2.kind == O2K_CONST_INT);

    EXPSET_TP result = 0;
    ssize_t iconVal = constAssertion->op2.u1.iconVal;

    // chkAssertionMask includes op1's lvAssertionDep
    EXPSET_TP chkAssertionMask = optGetVnMappedAssertions(constAssertion->op1.vn);
    if (!chkAssertionMask)
    {
        return result;
    }

    // Check each assertion in chkAssertionMask to see if it can be applied to constAssertion
    EXPSET_TP singleBit = 1;
    for (unsigned chkIndex = 1; chkIndex <= optAssertionCount; chkIndex++, singleBit <<= 1)
    {
        // Are there no more bits in chkAssertionMask ?
        if (!chkAssertionMask)
        {
            break;
        }

        if (!(singleBit & chkAssertionMask))
        {
            continue;
        }

        // Remove the singleBit from the chkAssertionMask.
        chkAssertionMask &= ~singleBit;

        // The impAssertion must be different from the const assertion.
        AssertionDsc* impAssertion = optGetAssertion(chkIndex);
        if (impAssertion == constAssertion)
        {
            continue;
        }

        // The impAssertion must be an assertion about the same local var.
        if (impAssertion->op1.vn != constAssertion->op1.vn)
        {
            continue;
        }

        bool usable = false;
        switch (impAssertion->op2.kind)
        {
        case O2K_SUBRANGE:
            // Is the const assertion's constant, within implied assertion's bounds?
            usable = ((iconVal >= impAssertion->op2.u2.loBound) && (iconVal <= impAssertion->op2.u2.hiBound));
            break;

        case O2K_CONST_INT:
            // Is the const assertion's constant equal/not equal to the implied assertion?
            usable = ((impAssertion->assertionKind == OAK_EQUAL) && (impAssertion->op2.u1.iconVal == iconVal)) ||
                     ((impAssertion->assertionKind == OAK_NOT_EQUAL) && (impAssertion->op2.u1.iconVal != iconVal));
            break;
        }

        if (usable)
        {       
            result |= optGetAssertionBit(chkIndex);
#ifdef  DEBUG
            if  (verbose)
            {
                AssertionDsc* firstAssertion = optGetAssertion(1);
                printf("\nCompiler::optImpliedByConstAssertion: constAssertion #%02d , implies assertion #%02d",
                        (constAssertion - firstAssertion) + 1,
                        (impAssertion - firstAssertion) + 1);
            }
#endif
        }
    }
    return result;
}


    
/*****************************************************************************
 *
 *  Given a copy assertion and a dependent assertion this method computes the
 *  set of implied assertions that are also true.
 *  For copy assertions, exact SSA num and LCL nums should match, because
 *  we don't have kill sets and we depend on their value num for dataflow.
 */

EXPSET_TP Compiler::optImpliedByCopyAssertion(AssertionDsc* copyAssertion, AssertionDsc* depAssertion)
{  
    noway_assert(copyAssertion->IsCopyAssertion());

    // Get the copyAssert's lcl/ssa nums.
    unsigned copyAssertLclNum = BAD_VAR_NUM;
    unsigned copyAssertSsaNum = SsaConfig::RESERVED_SSA_NUM;

    // Check if copyAssertion's op1 or op2 matches the depAssertion's op1.
    if (depAssertion->op1.lcl.lclNum == copyAssertion->op1.lcl.lclNum)
    {
        copyAssertLclNum = copyAssertion->op2.lcl.lclNum;
        copyAssertSsaNum = copyAssertion->op2.lcl.ssaNum;
    }
    else if (depAssertion->op1.lcl.lclNum == copyAssertion->op2.lcl.lclNum)
    {
        copyAssertLclNum = copyAssertion->op1.lcl.lclNum;
        copyAssertSsaNum = copyAssertion->op1.lcl.ssaNum;
    }
    // Check if copyAssertion's op1 or op2 matches the depAssertion's op2.
    else if (depAssertion->op2.kind == O2K_LCLVAR_COPY)
    {
        if (depAssertion->op2.lcl.lclNum == copyAssertion->op1.lcl.lclNum)
        {
            copyAssertLclNum = copyAssertion->op2.lcl.lclNum;
            copyAssertSsaNum = copyAssertion->op2.lcl.ssaNum;
        }
        else if (depAssertion->op2.lcl.lclNum == copyAssertion->op2.lcl.lclNum)
        {
            copyAssertLclNum = copyAssertion->op1.lcl.lclNum;
            copyAssertSsaNum = copyAssertion->op1.lcl.ssaNum;
        }
    }

    if (copyAssertLclNum == BAD_VAR_NUM ||
        copyAssertSsaNum == SsaConfig::RESERVED_SSA_NUM)
    {
        return 0;
    }

    // Get the depAssert's lcl/ssa nums.
    unsigned depAssertLclNum = BAD_VAR_NUM;
    unsigned depAssertSsaNum = SsaConfig::RESERVED_SSA_NUM;
    if ((depAssertion->op1.kind == O1K_LCLVAR) &&
        (depAssertion->op2.kind == O2K_LCLVAR_COPY))
    {
        if ((depAssertion->op1.lcl.lclNum == copyAssertion->op1.lcl.lclNum) ||
            (depAssertion->op1.lcl.lclNum == copyAssertion->op2.lcl.lclNum))
        {
            depAssertLclNum = depAssertion->op2.lcl.lclNum;
            depAssertSsaNum = depAssertion->op2.lcl.ssaNum;
        }
        else if ((depAssertion->op2.lcl.lclNum == copyAssertion->op1.lcl.lclNum) ||
                 (depAssertion->op2.lcl.lclNum == copyAssertion->op2.lcl.lclNum))
        {
            depAssertLclNum = depAssertion->op1.lcl.lclNum;
            depAssertSsaNum = depAssertion->op1.lcl.ssaNum;
        }
    }

    if (depAssertLclNum == BAD_VAR_NUM ||
        depAssertSsaNum == SsaConfig::RESERVED_SSA_NUM)
    {
        return 0;
    }

    // Is depAssertion a constant assignment of a 32-bit integer?
    // (i.e  GT_LVL_VAR X == GT_CNS_INT)
    bool depIsConstAssertion = ((depAssertion->assertionKind == OAK_EQUAL) && 
                                (depAssertion->op1.kind == O1K_LCLVAR) &&
                                (depAssertion->op2.kind == O2K_CONST_INT));

    // Search the assertion table for an assertion on op1 that matches depAssertion
    // The matching assertion is the implied assertion.
    EXPSET_TP result = 0;
    for (unsigned impIndex = 1; impIndex <= optAssertionCount; impIndex++)
    {
        AssertionDsc* impAssertion = optGetAssertion(impIndex);

        //  The impAssertion must be different from the copy and dependent assertions
        if (impAssertion == copyAssertion || impAssertion == depAssertion)
        {
            continue;
        }
               
        if (!AssertionDsc::SameKind(depAssertion, impAssertion))
        {
            continue;
        }

        bool op1MatchesCopy = (copyAssertLclNum == impAssertion->op1.lcl.lclNum) && (copyAssertSsaNum == impAssertion->op1.lcl.ssaNum);

        bool usable = false;
        switch (impAssertion->op2.kind)
        {
        case O2K_SUBRANGE:
            usable = op1MatchesCopy && ((impAssertion->op2.u2.loBound <= depAssertion->op2.u2.loBound) &&
                      (impAssertion->op2.u2.hiBound >= depAssertion->op2.u2.hiBound));
            break;

        case O2K_CONST_LONG:
            usable = op1MatchesCopy && (impAssertion->op2.lconVal == depAssertion->op2.lconVal);
            break;

        case O2K_CONST_DOUBLE:
            // Exact memory match because of positive and negative zero
            usable = op1MatchesCopy && (memcmp(&impAssertion->op2.dconVal, &depAssertion->op2.dconVal, sizeof(double)) == 0);
            break;

        case O2K_IND_CNS_INT:
            // This is the ngen case where we have a GT_IND of an address.
            noway_assert((impAssertion->op1.kind == O1K_EXACT_TYPE) || (impAssertion->op1.kind == O1K_SUBTYPE));

            __fallthrough;

        case O2K_CONST_INT:
            usable = op1MatchesCopy && (impAssertion->op2.u1.iconVal == depAssertion->op2.u1.iconVal);
            break;

        case O2K_LCLVAR_COPY:
            // Check if op1 of impAssertion matches copyAssertion and also op2 of impAssertion matches depAssertion.
            if (op1MatchesCopy && (depAssertLclNum == impAssertion->op2.lcl.lclNum && depAssertSsaNum == impAssertion->op2.lcl.ssaNum))
            {
                usable = true;
            }
            else
            {
                // Otherwise, op2 of impAssertion should match copyAssertion and also op1 of impAssertion matches depAssertion.
                usable = ((copyAssertLclNum == impAssertion->op2.lcl.lclNum && copyAssertSsaNum == impAssertion->op2.lcl.ssaNum) && 
                          (depAssertLclNum == impAssertion->op1.lcl.lclNum && depAssertSsaNum == impAssertion->op1.lcl.ssaNum));
            }
            break;
        }
            
        if (usable)
        {       
            result |= optGetAssertionBit(impIndex);

#ifdef  DEBUG
            if  (verbose)
            {
                AssertionDsc* firstAssertion = optGetAssertion(1);
                printf("\nCompiler::optImpliedByCopyAssertion: copyAssertion #%02d and depAssertion #%02d, implies assertion #%02d",
                       (copyAssertion - firstAssertion) + 1, (depAssertion  - firstAssertion) + 1, (impAssertion  - firstAssertion) + 1);
            }
#endif
            // If the depAssertion is a const assertion then any other assertions that it implies could also imply a subrange assertion.
            if (depIsConstAssertion)
            {
                result |= optImpliedByConstAssertion(impAssertion);
            }
        }

    }

    return result;
}

#include "dataflow.h"

/*****************************************************************************
 *
 * Dataflow visitor like callback so that all dataflow is in a single place
 *
 */
class AssertionPropFlowCallback
{
private:
    EXPSET_TP preMergeOut;
    EXPSET_TP postMergeOut;

    EXPSET_TP preMergeJumpDestOut;
    EXPSET_TP postMergeJumpDestOut;

    EXPSET_TP* mJumpDestOut;
    EXPSET_TP* mJumpDestGen;

    Compiler* m_pCompiler;

public:
    AssertionPropFlowCallback(Compiler* pCompiler, EXPSET_TP* jumpDestOut, EXPSET_TP* jumpDestGen)
        : mJumpDestOut(jumpDestOut)
        , mJumpDestGen(jumpDestGen)
        , m_pCompiler(pCompiler)
    {}

    // At the start of the merge function of the dataflow equations, initialize premerge state (to detect change.)
    void StartMerge(BasicBlock* block)
    {
        JITDUMP("AssertionPropCallback::StartMerge: BB%02u in -> %016I64X\n", block->bbNum, block->bbAssertionIn);
        preMergeOut = block->bbAssertionOut;
        preMergeJumpDestOut = mJumpDestOut[block->bbNum];
    }

    // During merge, perform the actual merging of the predecessor's (since this is a forward analysis) dataflow flags.
    void Merge(BasicBlock* block, BasicBlock* predBlock, flowList* preds)
    {
        EXPSET_TP* pAssertionOut = ((predBlock->bbJumpKind == BBJ_COND) && (predBlock->bbJumpDest == block)) ? &mJumpDestOut[predBlock->bbNum] : &predBlock->bbAssertionOut;
        JITDUMP("AssertionPropCallback::Merge     : BB%02u in -> %016I64X, pred BB%02u out -> %016I64X\n", block->bbNum, block->bbAssertionIn, predBlock->bbNum, predBlock->bbAssertionOut);
        block->bbAssertionIn &= *pAssertionOut;
    }

    // At the end of the merge store results of the dataflow equations, in a postmerge state.
    void EndMerge(BasicBlock* block)
    {
        JITDUMP("AssertionPropCallback::EndMerge  : BB%02u in -> %016I64X\n\n", block->bbNum, block->bbAssertionIn);
        postMergeOut = block->bbAssertionOut & (block->bbAssertionGen | block->bbAssertionIn);
        postMergeJumpDestOut = mJumpDestOut[block->bbNum] & (mJumpDestGen[block->bbNum] | block->bbAssertionIn);
    }

    // Check if anything changed by comparing premerge and postmerge states.
    bool Changed(BasicBlock* block)
    {
        JITDUMP("AssertionPropCallback::Changed  : BB%02u before out -> %016I64X; after out -> %016I64X;\n\t\tjumpDest before out -> %016I64X; jumpDest after out -> %016I64X;\n\n", block->bbNum, preMergeOut, postMergeOut, preMergeJumpDestOut, postMergeJumpDestOut);
        return (postMergeOut != preMergeOut) || (postMergeJumpDestOut != preMergeJumpDestOut);
    }

    // Finish any updates to the basic blocks after the merge.
    DataFlow::UpdateResult Update(BasicBlock* block)
    {
        block->bbAssertionOut = postMergeOut;
        mJumpDestOut[block->bbNum] = postMergeJumpDestOut;
        return DataFlow::ContinueAnalysis;
    }

    // Reset the analysis to start from the beginning. Unused for assertion prop.
    void Reset()
    {
        assert(false);
    }
};

/*****************************************************************************
 *
 *   Compute the assertions generated by each block.
 */
void Compiler::optComputeAssertionGen(EXPSET_TP* jumpDestGen)
{
    for (BasicBlock* block = fgFirstBB; block; block = block->bbNext)
    {
        EXPSET_TP valueGen = 0;
        EXPSET_TP jumpDestValueGen = 0;

        // Walk the statement trees in this basic block.
        for (GenTreePtr stmt = block->bbTreeList; stmt; stmt = stmt->gtNext)
        {
            noway_assert(stmt->gtOper == GT_STMT);

            for (GenTreePtr tree = stmt->gtStmt.gtStmtList; tree; tree = tree->gtNext)
            {
                // Store whatever we have accumulated into jumpDest edge's valueGen.
                if (tree->gtOper == GT_JTRUE)
                { 
                    jumpDestValueGen = valueGen;
                }
                if  (!tree->HasAssertion())
                {
                    continue;
                }

                // For regular trees, just update valueGen. For GT_JTRUE, for false part,
                // update valueGen and true part update jumpDestValueGen.
                unsigned int assertionIndex[2] = {
                    tree->GetAssertion(), 
                    (tree->OperGet() == GT_JTRUE) ? optFindComplementary(tree->GetAssertion()) : 0
                };

                for (unsigned i = 0; i < 2; ++i)
                {
                    if (assertionIndex[i] > 0)
                    {
                        // If GT_JTRUE, and true part use jumpDestValueGen.
                        EXPSET_TP& gen = (i == 0 && tree->OperGet() == GT_JTRUE) ? jumpDestValueGen : valueGen;
                        EXPSET_TP assertBits = optGetAssertionBit(assertionIndex[i]);
                        assertBits |= optImpliedAssertions(assertionIndex[i], gen);
                        gen |=  assertBits;
                    }
                }
            }
        }

        block->bbAssertionGen = valueGen;
        jumpDestGen[block->bbNum] = jumpDestValueGen;

#ifdef  DEBUG
        if  (verbose)
        {
            printf("\nBB%02u valueGen = %s", block->bbNum, genES2str(valueGen));
            if (block->bbJumpKind == BBJ_COND)
            {
                printf(" => BB%02u valueGen = %s,", block->bbJumpDest->bbNum, genES2str(jumpDestValueGen));
            }
        }
#endif
    }
}

/*****************************************************************************
 *
 *   Initialize the assertion data flow flags that will be propagated.
 */

void Compiler::optInitAssertionDataflowFlags(EXPSET_TP* jumpDestOut, EXPSET_TP* jumpDestGen)
{
    // Initially estimate the OUT sets to everything except killed expressions
    // Also set the IN sets to 1, so that we can perform the intersection
    for (BasicBlock* block = fgFirstBB; block; block = block->bbNext)
    {
        block->bbAssertionOut     = ((EXPSET_TP) -1);
        block->bbAssertionIn      = ((EXPSET_TP) -1);
        jumpDestOut[block->bbNum] = ((EXPSET_TP) -1);
    }
    // Compute the data flow values for all tracked expressions
    // IN and OUT never change for the initial basic block B1
    fgFirstBB->bbAssertionIn = 0;
}

/*****************************************************************************
 *
 *   Perform VN based i.e., data flow based assertion prop first because
 *   even if we don't gen new control flow assertions, we still propagate
 *   these first.
 *
 *   Returns the skipped next stmt if the current statement or next few
 *   statements got removed, else just returns the incoming stmt.
 */
GenTreePtr Compiler::optVNAssertionPropCurStmt(BasicBlock* block, GenTreePtr stmt)
{
    // TODO-Review: EH successor/predecessor iteration seems broken.
    // See: SELF_HOST_TESTS_ARM\jit\Directed\ExcepFilters\fault\fault.exe
    if (block->bbCatchTyp == BBCT_FAULT)
    {
        return stmt;
    }

    // Preserve the prev link before the propagation and morph.
    GenTreePtr prev = (stmt == block->firstStmt()) ? nullptr : stmt->gtPrev;

    // Perform VN based assertion prop first, in case we don't find
    // anything in assertion gen.
    optAssertionPropagatedCurrentStmt = false;
    for (GenTreePtr tree = stmt->gtStmt.gtStmtList; tree; tree = tree->gtNext)
    {
        GenTreePtr newTree = optAssertionProp(0, tree, stmt);
        if (newTree)
        {
            tree = newTree;
        }
    }

    if  (optAssertionPropagatedCurrentStmt)
    {
        fgMorphBlockStmt(block, stmt DEBUGARG("optVNAssertionPropCurStmt"));
        gtSetStmtInfo(stmt);
        fgSetStmtSeq(stmt);
    }

    // Check if propagation removed statements starting from current stmt.
    // If so, advance to the next good statement.
    GenTreePtr nextStmt = (prev == nullptr) ? block->firstStmt() : prev->gtNext;
    return nextStmt;
}

/*****************************************************************************
 *
 *   The entry point for assertion propagation
 */

void Compiler::optAssertionPropMain()
{
    if (fgSsaPassesCompleted == 0)
    {
        return;
    }
#ifdef DEBUG
    if (verbose) 
    {
        printf("*************** In optAssertionPropMain()\n");
        printf("Blocks/Trees at start of phase\n");
        fgDispBasicBlocks(true);
    }
#endif

    optAssertionInit(false);

    noway_assert(optAssertionCount == 0);

    // First discover all value assignments and record them in the table.
    for (BasicBlock* block = fgFirstBB; block; block = block->bbNext)
    {
        compCurBB = block;

        fgRemoveRestOfBlock = false;

        GenTreePtr stmt = block->bbTreeList;
        while (stmt)
        {
            // We need to remove the rest of the block.
            if (fgRemoveRestOfBlock)
            {
                fgRemoveStmt(block, stmt);
                stmt = stmt->gtNext;
                continue;
            }
            else
            {
                // Perform VN based assertion prop before assertion gen.
                GenTreePtr nextStmt = optVNAssertionPropCurStmt(block, stmt);

                // Propagation resulted in removal of the remaining stmts, perform it.
                if (fgRemoveRestOfBlock)
                {
                    stmt = stmt->gtNext;
                    continue;
                }

                // Propagation removed the current stmt or next few stmts, so skip them.
                if (stmt != nextStmt)
                {
                    stmt = nextStmt;
                    continue;
                }
            }

            // Perform assertion gen for control flow based assertions.
            for (GenTreePtr tree = stmt->gtStmt.gtStmtList; tree; tree = tree->gtNext)
            {
                optAssertionGen(tree);
            }

            // Advance the iterator
            stmt = stmt->gtNext;
        }
    }

    if  (!optAssertionCount)
    {
        return;
    }

#ifdef DEBUG
    fgDebugCheckLinks();
#endif

    // Allocate the bits for the predicate sensitive dataflow analysis
    EXPSET_TP* jumpDestGen = fgAllocateTypeForEachBlk<EXPSET_TP>(CMK_AssertionProp);
    optComputeAssertionGen(jumpDestGen);

    bbJtrueAssertionOut = fgAllocateTypeForEachBlk<EXPSET_TP>(CMK_AssertionProp);
    optInitAssertionDataflowFlags(bbJtrueAssertionOut, jumpDestGen);

    // Modified dataflow algorithm for available expressions.
    DataFlow flow(this);
    AssertionPropFlowCallback ap(this, bbJtrueAssertionOut, jumpDestGen);
    flow.ForwardAnalysis(ap);

    for (BasicBlock* block = fgFirstBB; block; block = block->bbNext)
    {
        // Compute any implied non-Null assertions for block->bbAssertionIn
        block->bbAssertionIn |= optImpliedByTypeOfAssertions(block->bbAssertionIn);
    }


#ifdef  DEBUG
    if  (verbose)
    {
        printf("\n");
        for (BasicBlock* block = fgFirstBB; block; block = block->bbNext)
        {
            printf("\nBB%02u", block->bbNum);
            printf(" valueIn  = %s", genES2str(block->bbAssertionIn));
            printf(" valueOut = %s", genES2str(block->bbAssertionOut));
            if (block->bbJumpKind == BBJ_COND)
            {
                printf(" => BB%02u", block->bbJumpDest->bbNum);
                printf(" valueOut= %s",  genES2str(bbJtrueAssertionOut[block->bbNum]));
            }
        }
        printf("\n");
    }
#endif // DEBUG

    // Perform assertion propagation (and constant folding)
    for (BasicBlock* block = fgFirstBB; block; block = block->bbNext)
    {
        EXPSET_TP assertions = block->bbAssertionIn;
#ifdef DEBUG
        if (verbose)
            printf("Assertions at start of BB%02u => %016I64X\n", block->bbNum, assertions);
#endif

        // TODO-Review: EH successor/predecessor iteration seems broken.
        // SELF_HOST_TESTS_ARM\jit\Directed\ExcepFilters\fault\fault.exe
        if (block->bbCatchTyp == BBCT_FAULT)
        {
            continue;
        }

        // Make the current basic block address available globally.
        compCurBB = block;
        fgRemoveRestOfBlock = false;

        // Walk the statement trees in this basic block
#if JIT_FEATURE_SSA_SKIP_DEFS
        GenTreePtr stmt = block->FirstNonPhiDef();
#else
        GenTreePtr stmt = block->bbTreeList;
#endif
        while (stmt)
        {
            noway_assert(stmt->gtOper == GT_STMT);

            // Propagation tells us to remove the rest of the block. Remove it.
            if (fgRemoveRestOfBlock)
            {
                fgRemoveStmt(block, stmt);
                stmt = stmt->gtNext;
                continue;
            }

            // Preserve the prev link before the propagation and morph, to check if propagation
            // removes the current stmt.
            GenTreePtr prev = (stmt == block->firstStmt()) ? nullptr : stmt->gtPrev;

            optAssertionPropagatedCurrentStmt = false;     // set to true if a assertion propagation took place
                                                           // and thus we must morph, set order, re-link
            for (GenTreePtr tree = stmt->gtStmt.gtStmtList; tree; tree = tree->gtNext)
            {
                GenTreePtr newTree = optAssertionProp(assertions, tree, stmt);

                if (newTree)
                {
                    assert(optAssertionPropagatedCurrentStmt == true);
                    tree = newTree;
                }

                // Is this an assignment to a local variable
                GenTreeLclVarCommon* lclVarTree = nullptr;

                // If this tree makes an assertion - make it available.
                if  (tree->HasAssertion())
                {
                    assertions |= optGetAssertionBit(tree->GetAssertion());

                    // Also include any implied assertions for the tree node.
                    assertions  |= optImpliedAssertions(tree->GetAssertion(), assertions);
#ifdef DEBUG
                    if (verbose)
                    {
                        printf(" + tree ");
                        printTreeID(tree);
                        printf(", index=#%02u => %016I64X\n", tree->GetAssertion(), assertions);
                    }
#endif
                }
            }

            if  (optAssertionPropagatedCurrentStmt)
            {
#ifdef DEBUG
                if (verbose)
                {
                    printf("Re-morphing this stmt:\n");
                    gtDispTree(stmt);
                    printf("\n");
                }
#endif
                // Re-morph the statement.
                fgMorphBlockStmt(block, stmt DEBUGARG("optAssertionPropMain"));

                // Recalculate the gtCostSz, etc...
                gtSetStmtInfo(stmt);

                // Re-thread the nodes
                fgSetStmtSeq(stmt);
            }

            // Check if propagation removed statements starting from current stmt.
            // If so, advance to the next good statement.
            GenTreePtr nextStmt = (prev == nullptr) ? block->firstStmt() : prev->gtNext;        
            stmt = (stmt == nextStmt) ? stmt->gtNext : nextStmt;
        }
        optAssertionPropagatedCurrentStmt = false;     // clear it back as we are done with stmts.
    }
    
#ifdef DEBUG
    fgDebugCheckBBlist();
    fgDebugCheckLinks();
#endif

    // Assertion propagation may have changed the reference counts 
    // We need to resort the variable table

    if (optAssertionPropagated)
    {
        lvaSortAgain = true;
    }
}


