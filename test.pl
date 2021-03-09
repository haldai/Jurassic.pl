:- ['jurassic.pl'].

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
 Basics
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
:- := println("Hello World!").
:-
    a := sqrt(2.0),
    := println(a).
:- jl_isdefined(a).
:- a := 1.
:- jl_isdefined(a).
:- := f(x) = x*transpose(x).
:- := @show(f([1,2,3,4,5])).
:- a := f([1,2,3,4,5]).
:- X = 2, := @show(a[1,X]).
:- X = 100, := @show(a[1,X]).
:- := cmd("fib(n) = n <= 1 ? 1 : fib(n-1) + fib(n-2)").
:- := @time(@show(fib(46))).
:-
    := cmd("function fib2(n)
          n <= 1 && return 1
          sum = 0
          while n > 1
          sum += fib2(n-1)
          n -= 2
          end
          return sum + 1
          end").
:- := @time(@show(fib2(46))).
:- jl_using("Pkg").
:- := 'Pkg'.status().

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/*
:- := "struct point
    x
    y
    end".
:- a := cmd("[point([1,2,3],[1,2,3]), point([2,3,4],[2,3,4])]").
:- := @show(a[2].x[1]).
*/
:- := @show(ccall(tuple([:clock, "libc.so.6"]), 'Int32', tuple([]))).
:- := cmd("function getenv(var::AbstractString)
         val = ccall((:getenv, \"libc.so.6\"),
          Cstring, (Cstring,), var)
         if val == C_NULL
         error(\"getenv: undefined variable: \", var)
         end
         unsafe_string(val)
         end").
:- X := getenv("SHELL").
:- := f(x) = sqrt(x) + x^2 + log(x) + 1.0.
:- X := f(2.0).
:- between(1,10,X),
    Y := f(X).
:- a := 1,
    X := a,
    b := X + 1,
    Y := b,
    Z := c.
:- a := array('Int64', undef, 2, 2).
:- X = a, := @show(X).
:- X = :a, := @show(X).
:- := f(x) = pi.*x.
:- X := f([1,2,3,4,5]).
:- := f(x) = x*transpose(x).
:- := @show(typeof(f([1,2,3]))),
    X := f([1,2,3]).
:- X := 1/0.
:- X := -1/0.
:- a := tuple([1,"I'm string!", tuple([2.0, :'I\'m a quoted symbol'])]), := @show(a).
:- := cmd("f(x) = (x, x^2, x^3)").
:- A = a, tuple([A, B, C]) := f(-2).
:- := @show(a).
:- a := 1.
:- X := [a, :a].
:- X := tuple([1+1, :a]).
:- jl_using("Plots").
:- := gr().
:- plt := plot(rand(10), kw(title, "10 Random numbers"), kw(fmt, :png), kw(show, false)).
:- := savefig(plt, "rand10.png").
:- := foo(x,y,z) = sum([x,y,z]).
:- := foo([1,2,3]).
:- := foo([1,2,3]...).
:- X := map(x ->> pi*x, [1,2,3,4,5]).
:- a := array('Float64', undef, 2, 2, 2).
:- := @show(a[1,:,:]).
:- jl_new_array(a, 'Int', undef, [2, 2, 2]).
:- := @show(a[1,:,:]).
:- a := array(union('Int64', 'Missing'), missing, 2, 2).
:- a[1, :] := [1,2].
:- := @show(a).
