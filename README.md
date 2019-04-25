# word2vec_neg
This version of word2vec(**word2vec_neg**) is modified based on the original version of [word2vec](https://github.com/dav/word2vec).

The idea of **word2vec_neg** is inspired by the KDD2018 best paper [***Real-time Personalization using Embeddings for Search Ranking at Airbnb***](http://delivery.acm.org/10.1145/3220000/3219885/p311-grbovic.pdf?ip=210.3.118.242&id=3219885&acc=OPENTOC&key=4D4702B0C3E38B35%2E4D4702B0C3E38B35%2E4D4702B0C3E38B35%2E054E54E275136550&__acm__=1556166101_672ca09c4ef3d25f5114863ff27c7c1f)。

The idea which inspires me in that paper is  as follows:

> due to random sampling of negatives, it is very likely that Dn contains mostly listings that are not from the same markets as listings in Dp . At each step, for a given central listing l, the positive context mostly consist of listings from the same market as l, while the negative context mostly consists of listings that are not from the same market as l. We found that this imbalance leads to learning sub-optimal within-market similarities. To address this issue we propose to add a set of random negatives Dmn, sampled from the market of the central listing l
>
> page 313 in airbnb paper

That is to say, when we choose negtive sampling in word2vec, the negtive samples shold not be far away from the positive samples, or the embedding vectors can not be differentiated, so we should choose negatives from same region, then word2vec will distinguish the differences between positives and negtives , and learn well. 

The format of the trainning data is:

```
xx xx xx ... xx NEG xx xx xx ... xx
xx xx xx ... xx NEG xx xx xx ... xx
...
xx xx xx ... xx NEG xx xx xx ... xx
```

Sequence before NEG is positives, and sequence after NEG is negtives.

This can be used to Recommend Systems.

How to use: git clone from [word2vec](https://github.com/dav/word2vec), and move word2vec_neg.c file to the /src folder ,then rename and replace the existing word2vec.c file, then `make` and `sh create-text8-vector-data.sh `. Attention：you should replace the training file `text8` in `\data` by your own data using the format of `xx xx xx ... xx NEG xx xx xx ... xx`

