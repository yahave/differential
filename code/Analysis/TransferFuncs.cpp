/*
 * CorrelatingTransformer.cpp
 *
 *  Created on: May 13, 2013
 *      Author: user
 */

#include "TransferFuncs.h"

#include <iostream>
using namespace std;

#define DEBUGCons   0
#define DEBUGExp    0
#define DEBUGGuard  0

//===----------------------------------------------------------------------===//
// Transfer functions.
//===----------------------------------------------------------------------===//

ostream& operator<<(ostream& os, const differential::ExpressionState &es) {
	os << es.e_ << es.s_ << es.ns_;
	return os;
}

namespace differential {

void TransferFuncs::AssumeTagEquivalence(environment &env, const string &name){
	tcons1 equal_cons = AnalysisUtils::GetEquivCons(env,name);
	state_ &= equal_cons;
	nstate_ &= equal_cons;
}

// forget all guard information and assume equivalence.
void TransferFuncs::AssumeGuardEquivalence(environment &env, string name){
	string tagged_name;
	Utils::Names(name,tagged_name);
	tcons1 equal_cons = AnalysisUtils::GetEquivCons(env,name);

	state_.Forget(name);
	state_.Forget(tagged_name);
	nstate_.Forget(name);
	nstate_.Forget(tagged_name);

	state_ &= equal_cons;
	nstate_ &= equal_cons;
}

ExpressionState TransferFuncs::VisitDeclRefExpr(DeclRefExpr* node) {
	ExpressionState result;
	if ( VarDecl* decl = dyn_cast<VarDecl>(node->getDecl()) ) {
		string type = decl->getType().getAsString();
		stringstream name;
		name << (tag_ ? Defines::kTagPrefix : "") << decl->getNameAsString();
		var v(name.str());
		result = texpr1(environment().add(&v,1,0,0),v);
	}
	expr_map_[node] = result;
	return result;
}

ExpressionState TransferFuncs::VisitCallExpr(CallExpr *node) {
	llvm::outs() << "Encountered function call: ";
	node->printPretty(llvm::outs(),AD.getContext(),0, PrintingPolicy(LangOptions()));
	llvm::outs() << "\n";
	return ExpressionState();

}

ExpressionState TransferFuncs::VisitParenExpr(ParenExpr *node) {
	Expr * sub = node->getSubExpr();
	Visit(sub);
	ExpressionState result = expr_map_[node] = expr_map_[sub];
	return result;
}


ExpressionState TransferFuncs::VisitImplicitCastExpr(ImplicitCastExpr * node) {
	Visit(node->getSubExpr());
	ExpressionState result = expr_map_[node->getSubExpr()];
	if ( VarDecl* decl = FindBlockVarDecl(node) ) {
		string type = decl->getType().getAsString();
		stringstream name;
		name << (tag_ ? Defines::kTagPrefix : "") << decl->getNameAsString();
		if (node->getCastKind() == CK_IntegralToBoolean && type == Defines::kGuardType) { // if (guard)
			var guard(name.str());
#if (DEBUGGuard)
			cerr << "\n------\nEncountered: " << name.str() << "\n";
#endif
			// nstate should only matter in VisitImplicitCastExpr and in VisitUnaryOperator
			// as they are the actual way that the fixed point algorithm sees conditionals
			// in the union program (eithre as (!Ret) <- unary not, or as (g) <- cast from integral to boolean)
			nstate_ = state_;
			environment env = result.e_.get_environment();
			state_.MeetGuard(tcons1(texpr1(env,guard) == AnalysisUtils::kOne));
			nstate_.MeetGuard(tcons1(texpr1(env,guard) == AnalysisUtils::kZero));
#if (DEBUGGuard)
			cerr << "State = " << state_ << ", NState = " << nstate_ << "\n------\n";
#endif
		}
	}
	expr_map_[node] = result;
	return result;
}

VarDecl* TransferFuncs::FindBlockVarDecl(Expr* node) {
	// Blast through casts and parentheses to find any DeclRefExprs that
	// refer to a block VarDecl.
	if ( DeclRefExpr* DR = dyn_cast<DeclRefExpr>(node->IgnoreParenCasts()) )
		if ( VarDecl* VD = dyn_cast<VarDecl>(DR->getDecl()) )
			return VD;
	return NULL;
}

ExpressionState TransferFuncs::VisitUnaryOperator(UnaryOperator* node) {
	Expr * sub = node->getSubExpr();
	texpr1 sub_expr = Visit(sub);
	ExpressionState result = sub_expr;
	VarDecl* decl = FindBlockVarDecl(sub);
	stringstream name;
	name << (tag_ ? Defines::kTagPrefix : "") << (decl ? decl->getNameAsString() : "");
	string type = (decl) ? decl->getType().getAsString() : "";
	var v(name.str());
	environment env = sub_expr.get_environment();

	switch ( node->getOpcode() ) {
	case UO_PostInc:
		state_.Assign(env,v,sub_expr + AnalysisUtils::kOne);
		nstate_.Assign(env,v,sub_expr + AnalysisUtils::kOne);
		result = (texpr1)(sub_expr);
		break;
	case UO_PreInc:
		state_.Assign(env,v,sub_expr + AnalysisUtils::kOne);
		nstate_.Assign(env,v,sub_expr + AnalysisUtils::kOne);
		result = (texpr1)(sub_expr + AnalysisUtils::kOne);
		break;
	case UO_PostDec:
		state_.Assign(env,v,sub_expr - AnalysisUtils::kOne);
		nstate_.Assign(env,v,sub_expr - AnalysisUtils::kOne);
		result = (texpr1)(sub_expr);
		break;
	case UO_PreDec:
		state_.Assign(env,v,sub_expr - AnalysisUtils::kOne);
		nstate_.Assign(env,v,sub_expr - AnalysisUtils::kOne);
		result = (texpr1)(sub_expr - AnalysisUtils::kOne);
		break;
	case UO_Minus:
		result = (texpr1)(-sub_expr);
		break;
	case UO_LNot:
	{
		// nstate should only matter in VisitImplicitCastExpr and in VisitUnaryOperator
		// as they are the actual way that the fixed point algorithm sees conditionals
		// in the union program (eithre as (!Ret) <- unary not, or as (g) <- cast from integral to boolean)
		if (type == Defines::kGuardType) {
			State tmp = nstate_;
			nstate_ = state_;
			state_ = tmp;
		}
		State tmp = result.s_;
		result.s_ = result.ns_;
		result.ns_ = tmp;
	}
	break;
	default:
		break;
	}
	expr_map_[node] = result;
	return result;
}

ExpressionState TransferFuncs::VisitBinaryOperator(BinaryOperator* node) {

	manager mgr = *(state_.mgr_ptr_);
	environment &env = state_.env_;
	Expr *lhs = node->getLHS(), *rhs = node->getRHS();
	Visit(lhs);
	Visit(rhs);
	ExpressionState left = expr_map_[lhs], right = expr_map_[rhs], result;
	texpr1 left_texpr = left, right_texpr = right;

#if (DEBUGExp)
	cerr << "Left = " << left << " \nRight = " << right << endl;
#endif

	VarDecl * left_var_decl_ptr = FindBlockVarDecl(lhs);

	stringstream left_var_name;
	if ( left_var_decl_ptr ) {
		left_var_name << (tag_ ? Defines::kTagPrefix : "") << left_var_decl_ptr->getNameAsString();
	}
	var left_var(left_var_name.str());

	if ( node->getOpcode() >= BO_Assign ) { // Making sure we are assigning to an integer
		if ( !left_var_decl_ptr || !left_var_decl_ptr->getType().getTypePtr()->isIntegerType() ) { // Array? or non Integer
			return left_texpr;
		}
	}

	left_texpr.extend_environment(AnalysisUtils::JoinEnvironments(left_texpr.get_environment(),right_texpr.get_environment()));
	right_texpr.extend_environment(AnalysisUtils::JoinEnvironments(left_texpr.get_environment(),right_texpr.get_environment()));

	set<abstract1> expr_abs_set, neg_expr_abs_set;
	switch ( node->getOpcode() ) {
	case BO_Assign:
	{
		if (isa<CallExpr>(rhs->IgnoreParenCasts())) { // don't break equivalence for function calls
			result = left_texpr;
			break;
		}
		bool is_guard = (left_var_decl_ptr->getType().getAsString() == Defines::kGuardType);
		state_.Assign(env,left_var,right_texpr,is_guard);
		// Assigning to guard variables needs special handling
		if ( is_guard ) {
			State tmp = right.s_;
			tmp.MeetGuard(tcons1((texpr1)left == AnalysisUtils::kOne));
			State ntmp = right.ns_;
			ntmp.MeetGuard(tcons1((texpr1)left == AnalysisUtils::kZero));
			state_ &= (tmp |= ntmp);
#if (DEBUGGuard)
			cerr << "Assigned to guard " << left_var << "\nState = " << state_ << " NState = " << nstate_ << "\n";
			getchar();
#endif
		}

		result = right_texpr;
		break;
	}

	case BO_AddAssign:
		// Var += Exp --> Substitute Var with (Var + Exp)
		state_.Assign(env,left_var,left_texpr+right_texpr);
	case BO_Add:
		result = texpr1(left_texpr+right_texpr);
		break;

	case BO_SubAssign:
		// Var -= Exp --> Substitute Var with (Var - Exp)
		state_.Assign(env,left_var,left_texpr-right_texpr);
	case BO_Sub:
		result = texpr1(left_texpr-right_texpr);
		break;

	case BO_MulAssign:
		// Var *= Exp --> Substitute Var with (Var * Exp)
		state_.Assign(env,left_var,left_texpr*right_texpr);
	case BO_Mul:
		result = texpr1(left_texpr*right_texpr);
		break;

	case BO_DivAssign:
		// Var /= Exp --> Substitute Var with (Var / Exp)
		state_.Assign(env,left_var,left_texpr/right_texpr);
	case BO_Div:
		result = texpr1(left_texpr/right_texpr);
		break;

	case BO_RemAssign:
		// Var %= Exp --> Substitute Var with (Var % Exp)
		state_.Assign(env,left_var,left_texpr%right_texpr);
	case BO_Rem:
		result = texpr1(left_texpr%right_texpr);
		break;

	case BO_EQ:
	{
		tcons1 constraint = (left_texpr == right_texpr);
		expr_abs_set.insert(AnalysisUtils::AbsFromConstraint(*state_.mgr_ptr_,constraint));
		AnalysisUtils::NegateConstraint(mgr,constraint,neg_expr_abs_set);
		result.s_.Assume(expr_abs_set);
		result.ns_.Assume(neg_expr_abs_set);
		break;
	}
	case BO_NE:
	{
		tcons1 constraint = (left_texpr == right_texpr);
		AnalysisUtils::NegateConstraint(mgr,constraint,expr_abs_set);
		neg_expr_abs_set.insert(AnalysisUtils::AbsFromConstraint(mgr,constraint));
		result.s_.Assume(expr_abs_set);
		result.ns_.Assume(neg_expr_abs_set);
		break;
	}
	case BO_GE:
	{
		tcons1 constraint = (left_texpr >= right_texpr);
		expr_abs_set.insert(AnalysisUtils::AbsFromConstraint(mgr,constraint));
		AnalysisUtils::NegateConstraint(mgr,constraint,neg_expr_abs_set);
		result.s_.Assume(expr_abs_set);
		result.ns_.Assume(neg_expr_abs_set);
		break;
	}
	case BO_GT:
	{
		tcons1 constraint = (left_texpr >= right_texpr + AnalysisUtils::kOne);
		expr_abs_set.insert(AnalysisUtils::AbsFromConstraint(mgr,constraint));
		AnalysisUtils::NegateConstraint(mgr,constraint,neg_expr_abs_set);
		result.s_.Assume(expr_abs_set);
		result.ns_.Assume(neg_expr_abs_set);
		break;
	}
	case BO_LE:
	{
		tcons1 constraint = (left_texpr <= right_texpr);
		expr_abs_set.insert(AnalysisUtils::AbsFromConstraint(mgr,constraint));
		AnalysisUtils::NegateConstraint(mgr,constraint,neg_expr_abs_set);
		result.s_.Assume(expr_abs_set);
		result.ns_.Assume(neg_expr_abs_set);
		break;
	}
	case BO_LT:
	{
		tcons1 constraint = (left_texpr <= right_texpr - AnalysisUtils::kOne);
		expr_abs_set.insert(AnalysisUtils::AbsFromConstraint(mgr,constraint));
		AnalysisUtils::NegateConstraint(mgr,constraint,neg_expr_abs_set);
		result.s_.Assume(expr_abs_set);
		result.ns_.Assume(neg_expr_abs_set);
		break;
	}
	case BO_LAnd:
	{
		result.s_ = left.s_;
		result.s_ &= right.s_;

		// !(L && R) = ~L or (L and ~R)
		// start off with (L and ~R)
		result.ns_ = left.s_;
		result.ns_ &= right.ns_;
		// now do ~L
		result.ns_ |= left.ns_;

		break;
	}
	case BO_LOr:
	{
		result.s_ = left.s_;
		result.s_ |= right.s_;

		// !(L || R) = ~L and ~R
		result.ns_ = left.ns_;
		result.ns_ &= right.ns_;

		break;
	}
	case BO_And:
	case BO_AndAssign:
	case BO_Or:
	case BO_OrAssign:
	case BO_Shl:
	case BO_ShlAssign:
	case BO_Shr:
	{
		/*
			if (!ExpRight.is_scalar())
				break;
			long Factor = 1;
			manager Mgr = state_.Abs.get_manager();
			while (state_.Abs.bound(Mgr,ExpRight) != interval(0,0)) {
				Factor *= 2;
				ExpRight = ExpRight - texpr1(state_.Abs.get_environment(),1);
			}
			return (texpr1)(ExpLeft * texpr1(state_.Abs.get_environment(),Factor));
			break;
		 */
	}
	case BO_ShrAssign:
	case BO_Xor:
	case BO_XorAssign:
	default:
		break;
	}
#if (DEBUGCons)
	cerr << "Result: " << result << endl;
	getchar();
#endif
	expr_map_[node] = result;
	return result;
}

ExpressionState TransferFuncs::VisitDeclStmt(DeclStmt* node) {
	for ( DeclStmt::const_decl_iterator iter = node->decl_begin(), end = node->decl_end(); iter != end; ++iter ) {
		if ( VarDecl *decl = cast<VarDecl>(*iter) ) {
			string type = decl->getType().getAsString();
			stringstream name;
			name << (tag_ ? Defines::kTagPrefix : "") << decl->getNameAsString();
			// correlation point variable - defined only to identify a point where the differential need be checked.
			if ( name.str().find(Defines::kCorrPointPrefix) == 0 ) {
				state_.at_diff_point_ = true;
				AD.Observer->ObserveAll(state_, node->getLocStart());
				// implement the partition-at-diff-point strategy
				if ( state_.partition_point == APAbstractDomain_ValueTypes::ValTy::PARTITION_AT_DIFF_POINT) {
					state_.Partition();
				}
			}
			if ( decl->getType().getTypePtr()->isIntegerType() ) { // apply to integers alone (this includes guards)
				// add the newly declared integer variable to the environment
				// this should be the ONLY place this is needed!
				var v(name.str());
				environment &env = state_.env_;
				if ( !env.contains(v) )
					env = env.add(&v,1,0,0);
				if ( Stmt* init = decl->getInit() ) {  // visit the subexpression to try and create an abstract expression
					state_.Assign(env,v,Visit(init), (type == Defines::kGuardType));
				} else { // if no init, assume that v == v'
					AssumeTagEquivalence(env,name.str());
				}
			}
		}
	}
	return ExpressionState();
}

ExpressionState TransferFuncs::VisitIntegerLiteral(IntegerLiteral * node) {
	long int value = node->getValue().getLimitedValue();
	ExpressionState result = texpr1(environment(), value );
	expr_map_[node] = result;
	return result;
}

struct Merge {
	void operator()(State& Dst, State& Src) {
		Dst |= Src;
	}
};

struct LowerOrEqual {
	bool operator()(State& Dst, State& Src) {
		//cerr << Dst << " <= " << Src << " = " << (Dst <= Src);
		return(Dst <= Src);
	}
};

} // end namespace differential



