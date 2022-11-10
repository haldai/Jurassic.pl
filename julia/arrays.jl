module JurassicArrays

export stack, unstack

# recursively unstack an array
unstack(a::Array) = begin
    return map(unstack, map(Array, collect(eachslice(a, dims=1))))
end
unstack(vs::Vector{T}) where T<:Real = vs

# Stacking nested vectors to array
# source: https://discourse.julialang.org/t/how-to-convert-vector-of-vectors-to-matrix/72609/23
unsqueeze(a::Array) = begin
    nsize = foldl(append!, size(a); init=[1])
    return reshape(a, Tuple(nsize))
end

stack(vs::Vector{T}) where T<:Vector = begin
    return reduce(vcat, map(unsqueeze, map(stack, vs)))
end
stack(vs::Vector{T}) where T<:Real = vs

end
