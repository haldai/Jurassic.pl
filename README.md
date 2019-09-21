# Jurassic.pl (侏逻辑)

Call Julia code from Prolog

# Prerequisite

- [Julia](https://github.com/JuliaLang)
- [SWI-Prolog (devel)](https://github.com/SWI-Prolog/swipl-devel)

This package is only tested on Linux, not sure if it will compile on MacOS
(maybe) or Windows (very unlikely).

# Build

Just run `make` directly.
``` shell
make
```

To debug the package, please uncomment the `#defeine JURASSIC_DEBUG` in
`c/jurassic.h`.

# Usage

Load `jurassic` module in SWI-Prolog:

``` prolog
?- use_module(jurassic).

```

Run Julia expression in Prolog with symbol `:=`:

``` prolog
?- := println("Hello World!").
% output
Hello World!
true.

?- a := sqrt(2.0),
|    := println(a).
1.4142135623730951
true.
```

Define a function and call a Julia macro:

``` prolog
?- := f(x) = x*transpose(x).
true.

?- := @show(f([1,2,3,4,5])).
f([1, 2, 3, 4, 5]) = [1 2 3 4 5; 2 4 6 8 10; 3 6 9 12 15; 4 8 12 16 20; 5 10 15 20 25]
true.
```

Define complicated functions with string:

``` prolog
?- := "fib(n) = n <= 1 ? 1 : fib(n-1) + fib(n-2)".
true.

?- := @time(@show(fib(46))).
fib(46) = 2971215073
  7.567550 seconds (3.97 k allocations: 228.485 KiB)
true.
```

Or faster:

``` prolog
?- := "function fib2(n)
|        n <= 1 && return 1
|        sum = 0
|        while n > 1
|            sum += fib2(n-1)
|            n -= 2
|        end
|        return sum + 1
|    end".
true.

?- := @time(@show(fib2(46))).
fib2(46) = 2971215073
  4.409316 seconds (60.55 k allocations: 3.183 MiB)
true.
```

Unify Prolog term with Julia expressions:

``` prolog
?- := f(x) = sqrt(x) + x^2 + log(x) + 1.0.
true.

?- X := f(2.0).
X = 7.10736074293304.

% Prolog and Julia work together
?- between(1,10,X), 
   := println(f(X)).
3.0
X = 1 ;
7.10736074293304
X = 2 ;
12.830663096236986
X = 3 ;
20.38629436111989
X = 4 ;
...
```

Tuple is defined with `tuple/1` predicate:

``` prolog
?- X := tuple([1,2,3,"I'm string!"]).
```

Currently, the unification only works for 1d-arrays:

``` prolog
?- := f(x) = pi.*x.
true.

?- X := f([1,2,3,4,5]).
X = [3.141592653589793, 6.283185307179586, 9.42477796076938, 12.566370614359172, 15.707963267948966].
```

Unification with 2d-array will fail:

``` prolog
?- := f(x) = x*transpose(x).
true.

?- X := f([1,2,3]).
[ERR] Cannot unify list with matrices and tensors!
false.
```

Import Julia packages or source files:

``` prolog
?- jl_using("Flux").
?- jl_include("my_source_file.jl").
```

More features to be added, e.g.:

- Multi-dimension arrays;
- Multi-threading.

# Acknowledgement

The `Jurassic.pl` package is inspired by
[real](https://www.swi-prolog.org/pack/file_details/real/prolog/real.pl)
(calling R from Prolog).

Another similar package is
[pljulia](https://www.swi-prolog.org/pack/file_details/pljulia/prolog/julia.pl),
unfortunately it is deprecated and only has limited functionalities.

# Author

__Wang-Zhou Dai__ ([homepage](http://daiwz.net))<br/>
Department of Computing<br/>
Imperial College London
