:- module(jurassic, [
                     jl_using/1,
                     jl_include/1,
                     jl_send_command/1,
                     jl_send_command_str/1,
                     jl_eval/2,
                     jl_eval_str/2,
                     ':='/1,
                     ':='/2,
                     op(950, fx, :=),
                     op(950, yfx, :=),
                     op(700, yfx, (+=)),
                     op(700, yfx, (-=)),
                     op(700, yfx, (*=)),
                     op(700, yfx, (/=)),
                     op(500, xfy, .+),
                     op(500, xfy, .-),                     
                     op(400, xfy, .*),
                     op(400, xfy, ./),
                     op(200, xfy, .^),
                     op(200, fx, @),
                     op(200, fx, :),
                     op(100, yf, []),
                     op(100, yf, ...)
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

%%	expand_dotted_name(+TermIn, -TermOut) is det.
%%
%%	Translate Atom1.Atom2 and Atom.Compound   into 'Atom1.Atom2' and
%%	'Atom1.Name'(Args).
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

join_dot(In, Out) :-
	compound_name_arguments(In, '.', [A,B]),
	atom(A),
	(   atom(B)
	->  atomic_list_concat([A,'.',B], Out)
	;   compound(B)
	->  compound_name_arguments(B, Name, Args),
	    atomic_list_concat([A,'.',Name], Name2),
	    compound_name_arguments(Out, Name2, Args)
	;   Out = In
	).

contains_dot(Term) :-
	compound(Term),
	(   compound_name_arity(Term, '.', 2)
	->  true
	;   arg(_, Term, Arg),
	    contains_dot(Arg)
	->  true
	).

user:goal_expansion(In, Out) :-
	contains_dot(In), !,
	expand_dotted_name(In, Out).

%%  expand '@' macros
%%	expand_macro_name(+TermIn, -TermOut) is det.
%%
%%	Translate @Atom1 into '@Atom1'(Args).
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

join_at(In, Out) :-
	compound_name_arguments(In, '@', [A]),
    (   compound(A)
    ->  compound_name_arguments(A, Name, Args),  
        atomic_list_concat(['@', Name], Name2),
        compound_name_arguments(Out, Name2, Args)
    ;   Out = In   
    ).

contains_at(Term) :-
	compound(Term),
	(   compound_name_arity(Term, '@', 1)
	->  true
	;   arg(_, Term, Arg),
	    contains_at(Arg)
	->  true
	).

user:goal_expansion(In, Out) :-
	contains_at(In), !,
	expand_macro_name(In, Out).

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

join_colon(In, Out) :-
	compound_name_arguments(In, ':', [A]),
    (   compound(A)
    ->  compound_name_arguments(A, Name, Args),  
        atomic_list_concat([':', Name], Name2),
        compound_name_arguments(Out, Name2, Args)
    ;   Out = In
    ).

contains_colon(Term) :-
	compound(Term),
	(   compound_name_arity(Term, ':', 1)
	->  true
	;   arg(_, Term, Arg),
	    contains_at(Arg)
	->  true
	).

user:goal_expansion(In, Out) :-
	contains_colon(In), !,
	expand_symb_name(In, Out).

:- load_foreign_library("lib/jurassic.so").