:- module(jurassic, [
                     jl_using/1,
                     jl_include/1,
                     jl_send_command/1,
                     jl_send_command_str/1,
                     jl_eval/2,
                     jl_eval_str/2,
                     jl_tuple_unify_str/2,
                     jl_tuple_unify/2,
                     ':='/1,
                     ':='/2,
                     op(950, fx, :=),
                     op(950, yfx, :=),
                     op(900, yfx, ->>),
                     op(700, yfx, (+=)),
                     op(700, yfx, (-=)),
                     op(700, yfx, (*=)),
                     op(700, yfx, (/=)),
                     op(500, xfy, .+),
                     op(500, xfy, .-),                     
                     op(400, xfy, .*),
                     op(400, xfy, ./),
                     op(200, xfy, .^),
                     op(100, xfy, ::),
                     op(100, yf, []),
                     op(100, xf, {}),
                     op(90, yf, ...),
                     op(50, fx, @),
                     op(50, fx, :)
                    ]).

/* Unary */
':='(X) :-
    string(X), !,
    jl_send_command_str(X).
':='(X) :-
    jl_send_command(X).
/* Binary */
':='(Y, X) :-
    ground(Y), !,
    := Y = X.
':='(Y, X) :-
    string(X), !,
    jl_eval_str(X, Y).
':='(Y, X) :-
    jl_eval(X, Y).

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 Syntax
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
    expand_dotted_name(TermIn, TermOut) :-
    compound(TermIn), !,
    (   join_dot(TermIn, Out)
    ->  TermOut = Out
    ;   contains_dot(TermIn)
    ->  compound_name_arguments(TermIn, Name, ArgsIn),
        maplist(expand_dotted_name, ArgsIn, ArgsOut),
        compound_name_arguments(TermOut, Name, ArgsOut)
    ;   TermOut = TermIn
    ).
expand_dotted_name(Term, Term).

join_dot(In, jl_field(A, B)) :-
	compound_name_arguments(In, '.', [A,B]).

contains_dot(Term) :-
    compound(Term),
    (   compound_name_arity(Term, '.', 2)
    ->  true
    ;   arg(_, Term, Arg),
        contains_dot(Arg)
    ->  true
    ).

expand_macro_name(TermIn, TermOut) :-
    compound(TermIn), !,
    (   join_at(TermIn, Out)
    ->  TermOut = Out
    ;   contains_at(TermIn)
    ->  compound_name_arguments(TermIn, Name, ArgsIn),
        maplist(expand_macro_name, ArgsIn, ArgsOut),
        compound_name_arguments(TermOut, Name, ArgsOut)
    ;   TermOut = TermIn
    ).
expand_macro_name(Term, Term).

join_at(In, jl_macro(A)) :-
	compound_name_arguments(In, '@', [A]).

contains_at(Term) :-
	compound(Term),
	(   compound_name_arity(Term, '@', 1)
	->  true
	;   arg(_, Term, Arg),
	    contains_at(Arg)
	->  true
	).

expand_symb_name(TermIn, TermOut) :-
    compound(TermIn), !,
    (   join_colon(TermIn, Out)
    ->  TermOut = Out
    ;   contains_at(TermIn)
    ->  compound_name_arguments(TermIn, Name, ArgsIn),
        maplist(expand_symb_name, ArgsIn, ArgsOut),
        compound_name_arguments(TermOut, Name, ArgsOut)
    ;   TermOut = TermIn
    ).
expand_symb_name(Term, Term).

join_colon(In, jl_symb(A)) :-
	compound_name_arguments(In, ':', [A]).

contains_colon(Term) :-
	compound(Term),
	(   compound_name_arity(Term, ':', 1)
	->  true
	;   arg(_, Term, Arg),
	    contains_at(Arg)
	->  true
	).

user:term_expansion((A ->> B), jl_inline(A, B)) :- !.
user:term_expansion(array:{Type, Dim}:(Init, Size), jl_new_array(Type, Dim, Init, [Size])) :-
    julia_type(Type), !.

/*
user:goal_expansion(tuple(List) := Expr, jl_tuple_unify_str(List, Expr)) :-
    string(Expr), !.
user:goal_expansion(tuple(List) := Expr, jl_tuple_unify(List, Expr)) :- !.
*/
user:goal_expansion(In, Out) :-
    contains_dot(In), !,
    expand_dotted_name(In, Out).
user:goal_expansion(In, Out) :-
	contains_at(In), !,
	expand_macro_name(In, Out).
user:goal_expansion(In, Out) :-
	contains_colon(In), !,
	expand_symb_name(In, Out).

:- load_foreign_library("lib/jurassic.so").