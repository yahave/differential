/*
 * CorrelatingTransformer.cpp
 *
 *  Created on: May 13, 2013
 *      Author: user
 */

#include "TransferFuncs.h"

#include <iostream>
using namespace std;

#define DEBUG		0
#define DEBUGCons   0
#define DEBUGExp    0
#define DEBUGGuard  0

//===----------------------------------------------------------------------===//
// Transfer functions.
//===----------------------------------------------------------------------===//

namespace differential {

ostream& operator<<(ostream& os, const ExpressionState &es){
	os << es.e_ << es.s_ << es.ns_;
	return os;
}

void TransferFuncs::AssumeTagEquivalence(State &state, const string &name){
	tcons1 equal_cons = AnalysisUtils::GetEquivCons(state.env_,name);
	state &= equal_cons;
}

// forget all guard information and assume equivalence.
void TransferFuncs::AssumeGuardEquivalence(State &state, string name){
	string tagged_name;
	Utils::Names(name,tagged_name);
	tcons1 equal_cons = AnalysisUtils::GetEquivCons(state.env_,name);
	state.Forget(name);
	state.Forget(tagged_name);
	state &= equal_cons;
}

ExpressionState TransferFuncs::VisitDeclRefExpr(DeclRefExpr* node) {
	ExpressionState result;
	if ( VarDecl* decl = dyn_cast<VarDecl>(node->getDecl()) ) {
		if (decl->getType().getTypePtr()->isIntegerType()) {
			string type = decl->getType().getAsString();
			stringstream name;
			name << (tag_ ? Defines::kTagPrefix : "") << decl->getNameAsString();
			var v(name.str());
			result = texpr1(environment().add(&v,1,0,0),v);
		}
	}
	expr_map_[node] = result;
	return result;
}

ExpressionState TransferFuncs::VisitCallExpr(CallExpr *node) {
	ExpressionState result;
	if ( node->getCallReturnType().getTypePtr()->isIntegerType() ) {
		string call;
		raw_string_ostream call_os(call);
		call_os << (tag_ ? Defines::kTagPrefix : "");
		node->printPretty(call_os,analysis_data_ptr_->getContext(),0, PrintingPolicy(LangOptions()));
		var v(call_os.str());
		environment env;
		result = texpr1(env.add(&v,1,0,0),v);
		// assume the value of the function call is the same in both versions (TODO: this may not always be the case)
		AssumeTagEquivalence(state_,call_os.str());
		AssumeTagEquivalence(nstate_,call_os.str());
	}
	expr_map_[node] = result;
	return result;
}

ExpressionState TransferFuncs::VisitParenExpr(ParenExpr *node) {
	return expr_map_[node] = BlockStmt_Visit(node->getSubExpr());
}

#define DEBUGVisitImplicitCastExpr 0
ExpressionState TransferFuncs::VisitImplicitCastExpr(ImplicitCastExpr * node) {
#if (DEBUGVisitImplicitCastExpr)
	cerr << "TransferFuncs::VisitImplicitCastExpr(";
	node->dump();
	cerr << "):\n";
#endif
	Visit(node->getSubExpr());
	ExpressionState result = expr_map_[node->getSubExpr()];
	if (isa<IntegerLiteral>(node->getSubExpr())) {
		return expr_map_[node] = result;
	}
	string name;
	raw_string_ostream name_os(name);
	name_os << (tag_ ? Defines::kTagPrefix : "");
	node->printPretty(name_os,analysis_data_ptr_->getContext(),0, PrintingPolicy(LangOptions()));
#if (DEBUGVisitImplicitCastExpr)
	cerr << "Encountered: " << name_os.str() << "\n";
#endif
	if (node->getCastKind() == CK_IntegralToBoolean) {
		environment env = result.e_.get_environment();
		VarDecl * decl = FindBlockVarDecl(node);
		if (decl && decl->getType().getAsString() == Defines::kGuardType) { // if (guard)
			// nstate should only matter in VisitImplicitCastExpr and in VisitUnaryOperator
			// as they are the actual way that the fixed point algorithm sees conditionals
			// in the union program (eithre as (!Ret) <- unary not, or as (g) <- cast from integral to boolean)
			nstate_ = state_;
			state_.MeetGuard(tcons1(texpr1(env,name_os.str()) == AnalysisUtils::kOne));
			nstate_.MeetGuard(tcons1(texpr1(env,name_os.str()) == AnalysisUtils::kZero));
		} else if (decl) { // if (something)
			// result.s_ = { v != 0 }, result.ns_ = { v == 0 }
			State s1,s2;
			s1.Meet(tcons1(texpr1(env,name_os.str()) > AnalysisUtils::kZero));
			s2.Meet(tcons1(texpr1(env,name_os.str()) < AnalysisUtils::kZero));
			result.s_ = s1.Join(s2);
			result.ns_.Meet(tcons1(texpr1(env,name_os.str()) == AnalysisUtils::kZero));
		}
#if (DEBUGVisitImplicitCastExpr)
		cerr << "State = " << state_ << ", NState = " << nstate_ << "\n------\n";
#endif
	}
	expr_map_[node] = result;
	return result;
}

VarDecl* TransferFuncs::FindBlockVarDecl(Expr* node) {
	// Blast through casts and parentheses to find any DeclRefExprs that
	// refer to a block VarDecl.
	if ( DeclRefExpr* decl_ref_expr = dyn_cast<DeclRefExpr>(node->IgnoreParenCasts()) )
		if ( VarDecl* decl = dyn_cast<VarDecl>(decl_ref_expr->getDecl()) )
			if (decl->getType().getTypePtr()->isIntegerType())
				return decl;
	return NULL;
}

ExpressionState TransferFuncs::VisitUnaryOperator(UnaryOperator* node) {
	UnaryOperator::Opcode opcode = node->getOpcode();
	Expr * sub = node->getSubExpr();
	ExpressionState result = Visit(sub);
	if (opcode == UO_Minus) {
		return (expr_map_[node] = (texpr1)(-result.e_));
	}
	VarDecl* decl = FindBlockVarDecl(sub);
	texpr1 sub_expr = result.e_;
	stringstream name;
	name << (tag_ ? Defines::kTagPrefix : "") << (decl ? decl->getNameAsString() : "");
	string type = (decl) ? decl->getType().getAsString() : "";
	var v(name.str());
	environment env = sub_expr.get_environment();

	switch ( opcode ) {
	case UO_LNot:
	{
		/**
		 * nstate should only matter in VisitImplicitCastExpr and in VisitUnaryOperator
		 * as they are the actual way that the fixed point algorithm sees conditionals
		 * in the union program (either as (!Ret) <- unary not, or as (g) <- cast from integral to boolean)
		 */
		if (type == Defines::kGuardType) {
			State tmp = nstate_;
			nstate_ = state_;
			state_ = tmp;
		}
		State tmp = result.s_;
		result.s_ = result.ns_;
		result.ns_ = tmp;
		break;
	}
	case UO_PostInc:
		state_.Assign(env,v,sub_expr + AnalysisUtils::kOne);
		nstate_.Assign(env,v,sub_expr + AnalysisUtils::kOne);
		break;
	case UO_PreInc:
		state_.Assign(env,v,sub_expr + AnalysisUtils::kOne);
		nstate_.Assign(env,v,sub_expr + AnalysisUtils::kOne);
		result = (texpr1)(sub_expr + AnalysisUtils::kOne); // the value is (sub-expression + 1)
		break;
	case UO_PostDec:
		state_.Assign(env,v,sub_expr - AnalysisUtils::kOne);
		nstate_.Assign(env,v,sub_expr - AnalysisUtils::kOne);
		break;
	case UO_PreDec:
		state_.Assign(env,v,sub_expr - AnalysisUtils::kOne);
		nstate_.Assign(env,v,sub_expr - AnalysisUtils::kOne);
		result = (texpr1)(sub_expr - AnalysisUtils::kOne); // the value is (sub-expression - 1)
		break;
	default:
		assert(0 && "unknown unary operator");
		break;
	}
	return (expr_map_[node] = result);
}

/**
 * this method actually is the entry point for visiting if, for, while and switch statements.
 */
ExpressionState TransferFuncs::VisitConditionVariableInit(Stmt *node) {
	return Visit(node);
}

ExpressionState TransferFuncs::VisitIfStmt(IfStmt* node) {
	BlockStmt_Visit(node->getCond());
	ExpressionState result = expr_map_[node->getCond()];
//	cerr << "Meeting State " << state_ << " with " << result.s_;
	state_.Meet(result.s_);
//	cerr << " Result = " << state_;
//	cerr << "Meeting NState " << nstate_ << " with " << result.ns_;
	nstate_.Meet(result.ns_);
//	cerr << " Result = " << nstate_;
	return result;
}

ExpressionState TransferFuncs::VisitBinaryOperator(BinaryOperator* node) {
	manager mgr = *(state_.mgr_ptr_);
	environment &env = state_.env_;
	Expr *lhs = node->getLHS(), *rhs = node->getRHS();
	BlockStmt_Visit(lhs);
	BlockStmt_Visit(rhs);
	ExpressionState left = expr_map_[lhs], right = expr_map_[rhs], result;
	texpr1 left_texpr = left, right_texpr = right;
#if (DEBUGExp)
	cerr << "Left = " << left << " \nRight = " << right << '\n';
#endif

	VarDecl * left_var_decl_ptr = FindBlockVarDecl(lhs);

	if ( node->getOpcode() >= BO_Assign ) { // Making sure we are assigning to an integer
		if ( !left_var_decl_ptr || !left_var_decl_ptr->getType().getTypePtr()->isIntegerType() ) { // Array? or non Integer
			return left_texpr;
		}
	}

	stringstream left_var_name;
	if ( left_var_decl_ptr ) {
		left_var_name << (tag_ ? Defines::kTagPrefix : "") << left_var_decl_ptr->getNameAsString();
	}
	var left_var(left_var_name.str());

	left_texpr.extend_environment(AnalysisUtils::JoinEnvironments(left_texpr.get_environment(),right_texpr.get_environment()));
	right_texpr.extend_environment(AnalysisUtils::JoinEnvironments(left_texpr.get_environment(),right_texpr.get_environment()));

	set<abstract1> expr_abs_set, neg_expr_abs_set;
	switch ( node->getOpcode() ) {
	case BO_Assign:
	{
		bool is_guard = (left_var_decl_ptr->getType().getAsString() == Defines::kGuardType);
		if (!is_guard && rhs->isKnownToHaveBooleanValue()) { // take care of cases like: int x = (y < z);
			State s1 = state_, s2 = state_;
			s1.Assign(env,left_var,AnalysisUtils::kOne,false);
			s1.Meet(right.s_);
			s2.Assign(env,left_var,AnalysisUtils::kZero,false);
			s2.Meet(right.ns_);
			state_ = s1.Join(s2);
		} else {
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
	case BO_And:
	case BO_AndAssign:
	case BO_Or:
	case BO_OrAssign:
	case BO_Shl:
	case BO_ShlAssign:
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
			/**
			 * correlation point variable - defined only to identify a point where
			 * the differential need be checked.
			 */
			if ( name.str().find(Defines::kCorrPointPrefix) == 0 ) {
				state_.at_diff_point_ = true;
				analysis_data_ptr_->Observer->ObserveAll(state_, node->getLocStart());
				// implement the partition-at-corr-point strategy
				if ( state_.partition_point_ == AnalysisConfiguration::PARTITION_AT_CORR_POINT) {
					state_.Partition();
				}
			}

			if ( decl->getType().getTypePtr()->isIntegerType() ) { // apply to integers alone (this includes guards)
				manager mgr = *(state_.mgr_ptr_);
				var v(name.str());
				/* first forget the variable (in case its defined in a loop) */
				AbstractSet abstracts = state_.abs_set_;
				state_.abs_set_.clear();
				for (AbstractSet::const_iterator iter = abstracts.begin(), end = abstracts.end();  iter != end; ++iter) {
					if (iter->vars.abstract()->get_environment().contains(v) &&
						!iter->vars.abstract()->is_variable_unconstrained(mgr,v)) {
						abstract1 abs(*(iter->vars.abstract()));
						abs = abs.forget(mgr,v,true);
						state_.abs_set_.insert(Abstract2(abs,iter->guards));
					} else if (iter->guards.abstract()->get_environment().contains(v) &&
							   !iter->guards.abstract()->is_variable_unconstrained(mgr,v)) {
						abstract1 abs(*(iter->guards.abstract()));
						abs = abs.forget(mgr,v,true);
						state_.abs_set_.insert(Abstract2(iter->vars,abs));
					} else {
						state_.abs_set_.insert(*iter);
					}
				}
				/**
				 * add the newly declared integer variable to the environment.
				 * this should be the ONLY place this is needed!
				 */
				environment &env = state_.env_;
				if ( !env.contains(v) )
					env = env.add(&v,1,0,0);
				if ( Stmt* init = decl->getInit() ) {  // visit the subexpression to try and create an abstract expression
					if (type != Defines::kGuardType && decl->getInit()->isKnownToHaveBooleanValue()) {
						// take care of cases like: int x = (y < z);
						ExpressionState es = Visit(init);
						State s1 = state_, s2 = state_;
						s1.Assign(env,v,AnalysisUtils::kOne,false);
						s1.Meet(es.s_);
						s2.Assign(env,v,AnalysisUtils::kZero,false);
						s2.Meet(es.ns_);
						state_ = s1.Join(s2);
					} else {
						state_.Assign(env,v,Visit(init), (type == Defines::kGuardType));
					}
				} else { // if no init, assume that v == v'
//					AssumeTagEquivalence(state_,name.str());
//					AssumeTagEquivalence(nstate_,name.str());
				}
			}
		}
	}
	return ExpressionState();
}

#define DEBUGVisitIntegerLiteral 0
ExpressionState TransferFuncs::VisitIntegerLiteral(IntegerLiteral * node) {
	long int value = node->getValue().getLimitedValue();
#if(DEBUGVisitIntegerLiteral)
	cerr << "TransferFuncs::VisitIntegerLiteral: value = " << value << '\n';
#endif
	ExpressionState result = texpr1(environment(), value );
	expr_map_[node] = result;
	return result;
}

ExpressionState TransferFuncs::VisitCharacterLiteral(CharacterLiteral * node) {
	long int value = node->getValue();
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




