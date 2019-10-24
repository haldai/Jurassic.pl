pl2jl(Expr, Input) :-
    jl_tokenise(Input, Tokens),
    jl_parse(Tokens, Expr).

dot(.) --> [.].
comma(,) --> [,].

operator(=) --> [=].
operator(:=) --> [:, =].

%% tokens
type('Int') --> ['Int'].
type('Real') --> ['Real'].

type('Int16') --> ['Int16'].
type('Int32') --> ['Int32'].
type('Int64') --> ['Int64'].

type('Float16') --> ['Float16'].
type('Float32') --> ['Float32'].
type('Float64') --> ['Float64'].
