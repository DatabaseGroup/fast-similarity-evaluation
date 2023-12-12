use_module(lambda).

onesided_transformation(qgram).
onesided_transformation(preorder-traversal).
onesided_transformation(postorder-traversal).
onesided_transformation(labelset).
onesided_transformation(onegram).

onesided_transformable(A) :- onesided_transformation(A).
onesided_transformable(compose(A,B)) :- onesided_transformable(A), onesided_transformable(B).

lb(qgc,sed,qgram,\T^(*(q, T))).
lb(sed,ted,preorder-traversal,id).
lb(sed,ted,postorder-traversal,id).
lb(hd,ted,labelset,\T^(*(2, T))).
lb(jo,jaro,onegram,id).

ub(lcted,ted,id,id).
ub(cted,lcted,id,id).

lboundable(A,B,F,G) :- lb(A,B,F,G).
lboundable(A,C,compose(H,F),compose(I,G)) :- lb(A,B,F,G), lboundable(B,C,H,I).

uboundable(A,B,F,G) :- ub(A,B,F,G).
uboundable(A,C,compose(H,F),compose(I,G)) :- ub(A,B,F,G), uboundable(B,C,H,I).

eqo(qgc, \R^S^T^(max(R, S)-q*T), \R^T^(R - q*T)).
eqo(hd, \R^S^T^(/(R + S - T, 2)), \R^T^(R - (T + 1))).
eqo(jo, \R^S^T^((R*S) * (3*T - 1) / (R + S)), \R^T^((3*T-2) * R)).

indexable(B,ssj,F,G,O,L) :- lboundable(A,B,F,G), onesided_transformable(F), eqo(A,O,L).
