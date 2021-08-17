:- module(jurassic, [
                     jl_using/1,
                     jl_include/1,
                     jl_send_command/1,
                     jl_send_command_str/1,
                     jl_eval/2,
                     jl_eval_str/2,
                     jl_disp/1,
                     jl_show/1,
                     jl_tuple_unify_str/2,
                     jl_tuple_unify/2,
                     jl_isdefined/1,
                     jl_new_array/4,
                     jl_declare_function/3,
                     jl_declare_macro_function/4,                     
                     ':='/1,
                     ':='/2,
                     '$='/2,
                     op(950, fx, :=),
                     op(950, yfx, :=),
                     op(950, yfx, $=),
                     op(900, yfx, ~),
                     op(900, yfx, '->>'),
                     op(700, yfx, (+=)),
                     op(700, yfx, (-=)),
                     op(700, yfx, (*=)),
                     op(700, yfx, (/=)),
                     op(700, yfx, (^=)),
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
                     op(50, fx, :),
                     op(50, fx, $)
                    ]).

/* Unary */
':='(X) :-
    string(X), !,
    jl_send_command_str(X).
':='(X) :-
    jl_send_command(X).
/* Binary */
':='(Y, X) :-
    % assign function returns to a tuple
    compound(Y), Y = tuple(Z),
    jl_tuple_unify(tuple(Z), X), !.
':='(Y, X) :-
    ground(Y), !,
    := Y = X.
':='(Y, str(X)) :-
    string(X), !,
    jl_eval_str(X, Y).
':='(Y, X) :-
    jl_eval(X, Y).
/* Meta-programming: assign Julia variable Y with QuoteNode of X (without evaluation) */
'$='(Y, X) :-
    ground(Y), !,
    := Y = $(X).

/* array init */
jl_new_array(Name, Type, Init, Size) :-
    length(Size, Dim),
    list_tuple(Size, Size_Tuple), !,
    swritef(Str, '%w = Array{%w, %w}(%w, %w)', [Name, Type, Dim, Init, Size_Tuple]),
    := Str.

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

join_dot(In, Out) :- % the second argument should be quotenode
	compound_name_arguments(In, '.', [A,B]),
    compound(B),
    compound_name_arguments(B, Pred, ArgB),
    Pred \= [], !,
    Out =.. [call, jl_field(A, :Pred)|ArgB].
join_dot(In, jl_field(A, :B)) :- % the second argument should be quotenode
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

expand_array_init(TermIn, TermOut) :-
    compound(TermIn), !,
    (   init_array(TermIn, Out)
    ->  TermOut = Out
    ;   contains_array(TermIn)
    ->  compound_name_arguments(TermIn, Name, ArgsIn),
        maplist(expand_array_init, ArgsIn, ArgsOut),
        compound_name_arguments(TermOut, Name, ArgsOut)
    ;   TermOut = TermIn
    ).
expand_array_init(Term, Term).

init_array(In, Out) :-
    compound_name_arguments(In, array, [Type, Init|Size]),
    length(Size, Dim),
    Out =.. [call, curly('Array', Type, Dim), Init|Size].

contains_array(Term) :-
    compound(Term),
    (   compound_name_arity(Term, array, _)
    ->  true
    ;   arg(_, Term, Arg),
        contains_array(Arg)
    ->  true
    ).

%% Union{T1, T2, ...}
expand_union_init(TermIn, TermOut) :-
    compound(TermIn), !,
    (   init_union(TermIn, Out)
    ->  TermOut = Out
    ;   contains_union(TermIn)
    ->  compound_name_arguments(TermIn, Name, ArgsIn),
        maplist(expand_union_init, ArgsIn, ArgsOut),
        compound_name_arguments(TermOut, Name, ArgsOut)
    ;   TermOut = TermIn
    ).
expand_union_init(Term, Term).

init_union(In, Out) :-
    compound_name_arguments(In, union, Types),
    Out =.. [curly, 'Union'|Types].

contains_union(Term) :-
    compound(Term),
    (   compound_name_arity(Term, union, _)
    ->  true
    ;   arg(_, Term, Arg),
        contains_union(Arg)
    ->  true
    ).

expand_inline_init(TermIn, TermOut) :-
    compound(TermIn), !,
    (   init_inline(TermIn, Out)
    ->  TermOut = Out
    ;   contains_inline(TermIn)
    ->  compound_name_arguments(TermIn, Name, ArgsIn),
        maplist(expand_inline_init, ArgsIn, ArgsOut),
        compound_name_arguments(TermOut, Name, ArgsOut)
    ;   TermOut = TermIn
    ).
expand_inline_init(Term, Term).

init_inline(In, jl_inline(A, B)) :-
    compound_name_arguments(In, '->>', [A, B]).

contains_inline(Term) :-
    compound(Term),
    (   compound_name_arity(Term, '->>', 2)
    ->  true
    ;   arg(_, Term, Arg),
        contains_inline(Arg)
    ->  true
    ).

%% Turn list to tuple
list_tuple([A], (A)). 
list_tuple([A,B|L], (A,R)) :-
    list_tuple([B|L], R).

user:goal_expansion(In, Out) :-
    contains_dot(In), !,
    expand_dotted_name(In, Out).
user:goal_expansion(In, Out) :-
    contains_at(In), !,
    expand_macro_name(In, Out).
user:goal_expansion(In, Out) :-
    contains_union(In),
    expand_union_init(In, Out).
user:goal_expansion(In, Out) :-
    contains_array(In),
    expand_array_init(In, Out).
user:goal_expansion(In, Out) :-
    contains_inline(In),
    expand_inline_init(In, Out).

:- load_foreign_library("lib/jurassic.so").
:- at_halt(halt_hooks).

halt_hooks :-
    write("Halt embedded Julia ..."),
    jl_embed_halt,
    unload_foreign_library("lib/jurassic.so"),
    writeln("Done").

/* display julia variable */
jl_disp(X) :-
    := display(X), nl.
jl_show(X) :-
    := @show(X).
