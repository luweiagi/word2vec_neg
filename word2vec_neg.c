//  Copyright 2013 Google Inc. All Rights Reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#include <time.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
// 加入bool类型 lw
#include <stdbool.h>
#include <unistd.h>

#define MAX_STRING 100
#define EXP_TABLE_SIZE 1000
#define MAX_EXP 6
#define MAX_SENTENCE_LENGTH 1000
// 句子的最大负采样单词个数 lw
#define MAX_SENTENCE_NEG_LENGTH 1500
#define MAX_CODE_LENGTH 40

const int vocab_hash_size = 30000000;  // Maximum 30 * 0.7 = 21M words in the vocabulary

typedef float real;                    // Precision of float numbers

struct vocab_word {
    long long cn;
    int *point;
    char *word, *code, codelen;
};

char train_file[MAX_STRING], output_file[MAX_STRING];
char save_vocab_file[MAX_STRING], read_vocab_file[MAX_STRING];
struct vocab_word *vocab;
int binary = 0, cbow = 1, debug_mode = 2, window = 5, min_count = 5, num_threads = 12, min_reduce = 1;
int *vocab_hash;
long long vocab_max_size = 1000, vocab_size = 0, layer1_size = 100;
long long train_words = 0, word_count_actual = 0, iter = 5, file_size = 0, classes = 0;
real alpha = 0.025, starting_alpha, sample = 1e-3;
real *syn0, *syn1, *syn1neg, *expTable;
clock_t start;

int hs = 0, negative = 5;
const int table_size = 1e8;
int *table;

void InitUnigramTable() {
  int a, i;
  double train_words_pow = 0;
  double d1, power = 0.75;
  table = (int *)malloc(table_size * sizeof(int));
  for (a = 0; a < vocab_size; a++) train_words_pow += pow(vocab[a].cn, power);
  i = 0;
  d1 = pow(vocab[i].cn, power) / train_words_pow;
  for (a = 0; a < table_size; a++) {
    table[a] = i;
    if (a / (double)table_size > d1) {
      i++;
      d1 += pow(vocab[i].cn, power) / train_words_pow;
    }
    if (i >= vocab_size) i = vocab_size - 1;
  }
}

// Reads a single word from a file, assuming space + tab + EOL to be word boundaries
void ReadWord(char *word, FILE *fin) {
  int a = 0, ch;
  while (!feof(fin)) {
    ch = fgetc(fin);
    if (ch == 13) continue;
    if ((ch == ' ') || (ch == '\t') || (ch == '\n')) {
      if (a > 0) {
        if (ch == '\n') ungetc(ch, fin);
        break;
      }
      if (ch == '\n') {
        strcpy(word, (char *)"</s>");
        return;
      } else continue;
    }
    word[a] = ch;
    a++;
    if (a >= MAX_STRING - 1) a--;   // Truncate too long words
  }
  word[a] = 0;
}

// Returns hash value of a word
int GetWordHash(char *word) {
  unsigned long long a, hash = 0;
  for (a = 0; a < strlen(word); a++) hash = hash * 257 + word[a];
  hash = hash % vocab_hash_size;
  return hash;
}

// Returns position of a word in the vocabulary; if the word is not found, returns -1
int SearchVocab(char *word) {
  unsigned int hash = GetWordHash(word);
  while (1) {
    if (vocab_hash[hash] == -1) return -1;
    if (!strcmp(word, vocab[vocab_hash[hash]].word)) return vocab_hash[hash];
    hash = (hash + 1) % vocab_hash_size;
  }
  return -1;
}

// Reads a word and returns its index in the vocabulary
int ReadWordIndex(FILE *fin) {
  char word[MAX_STRING];
  ReadWord(word, fin);
  if (feof(fin)) return -1;
  // 如果遇到"NEG"就返回-2 lw
  if (!strcmp(word, "NEG")) {
    return -2;
  }
  return SearchVocab(word);
}

// Adds a word to the vocabulary
int AddWordToVocab(char *word) {
  unsigned int hash, length = strlen(word) + 1;
  if (length > MAX_STRING) length = MAX_STRING;
  vocab[vocab_size].word = (char *)calloc(length, sizeof(char));
  strcpy(vocab[vocab_size].word, word);
  vocab[vocab_size].cn = 0;
  vocab_size++;
  // Reallocate memory if needed
  if (vocab_size + 2 >= vocab_max_size) {
    vocab_max_size += 1000;
    vocab = (struct vocab_word *)realloc(vocab, vocab_max_size * sizeof(struct vocab_word));
  }
  hash = GetWordHash(word);
  while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
  vocab_hash[hash] = vocab_size - 1;
  return vocab_size - 1;
}

// Used later for sorting by word counts
int VocabCompare(const void *a, const void *b) {
  return ((struct vocab_word *)b)->cn - ((struct vocab_word *)a)->cn;
}

// Sorts the vocabulary by frequency using word counts
void SortVocab() {
  int a, size;
  unsigned int hash;
  // Sort the vocabulary and keep </s> at the first position
  qsort(&vocab[1], vocab_size - 1, sizeof(struct vocab_word), VocabCompare);
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  size = vocab_size;
  train_words = 0;
  for (a = 0; a < size; a++) {
    // Words occuring less than min_count times will be discarded from the vocab
    if ((vocab[a].cn < min_count) && (a != 0)) {
      vocab_size--;
      free(vocab[a].word);
    } else {
      // Hash will be re-computed, as after the sorting it is not actual
      hash=GetWordHash(vocab[a].word);
      while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
      vocab_hash[hash] = a;
      train_words += vocab[a].cn;
    }
  }
  vocab = (struct vocab_word *)realloc(vocab, (vocab_size + 1) * sizeof(struct vocab_word));
  // Allocate memory for the binary tree construction
  for (a = 0; a < vocab_size; a++) {
    vocab[a].code = (char *)calloc(MAX_CODE_LENGTH, sizeof(char));
    vocab[a].point = (int *)calloc(MAX_CODE_LENGTH, sizeof(int));
  }
}

// Reduces the vocabulary by removing infrequent tokens
void ReduceVocab() {
  int a, b = 0;
  unsigned int hash;
  for (a = 0; a < vocab_size; a++) if (vocab[a].cn > min_reduce) {
      vocab[b].cn = vocab[a].cn;
      vocab[b].word = vocab[a].word;
      b++;
    } else free(vocab[a].word);
  vocab_size = b;
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  for (a = 0; a < vocab_size; a++) {
    // Hash will be re-computed, as it is not actual
    hash = GetWordHash(vocab[a].word);
    while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
    vocab_hash[hash] = a;
  }
  fflush(stdout);
  min_reduce++;
}

// Create binary Huffman tree using the word counts
// Frequent words will have short uniqe binary codes
void CreateBinaryTree() {
  long long a, b, i, min1i, min2i, pos1, pos2, point[MAX_CODE_LENGTH];
  char code[MAX_CODE_LENGTH];
  long long *count = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));
  long long *binary = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));
  long long *parent_node = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));
  for (a = 0; a < vocab_size; a++) count[a] = vocab[a].cn;
  for (a = vocab_size; a < vocab_size * 2; a++) count[a] = 1e15;
  pos1 = vocab_size - 1;
  pos2 = vocab_size;
  // Following algorithm constructs the Huffman tree by adding one node at a time
  for (a = 0; a < vocab_size - 1; a++) {
    // First, find two smallest nodes 'min1, min2'
    if (pos1 >= 0) {
      if (count[pos1] < count[pos2]) {
        min1i = pos1;
        pos1--;
      } else {
        min1i = pos2;
        pos2++;
      }
    } else {
      min1i = pos2;
      pos2++;
    }
    if (pos1 >= 0) {
      if (count[pos1] < count[pos2]) {
        min2i = pos1;
        pos1--;
      } else {
        min2i = pos2;
        pos2++;
      }
    } else {
      min2i = pos2;
      pos2++;
    }
    count[vocab_size + a] = count[min1i] + count[min2i];
    parent_node[min1i] = vocab_size + a;
    parent_node[min2i] = vocab_size + a;
    binary[min2i] = 1;
  }
  // Now assign binary code to each vocabulary word
  for (a = 0; a < vocab_size; a++) {
    b = a;
    i = 0;
    while (1) {
      code[i] = binary[b];
      point[i] = b;
      i++;
      b = parent_node[b];
      if (b == vocab_size * 2 - 2) break;
    }
    vocab[a].codelen = i;
    vocab[a].point[0] = vocab_size - 2;
    for (b = 0; b < i; b++) {
      vocab[a].code[i - b - 1] = code[b];
      vocab[a].point[i - b] = point[b] - vocab_size;
    }
  }
  free(count);
  free(binary);
  free(parent_node);
}

void LearnVocabFromTrainFile() {
  char word[MAX_STRING];
  FILE *fin;
  long long a, i;
  // 当遇到NEG标签时，neg_flag置为真，停止录入单词，略过，直到开始下一行
  bool neg_flag = false;
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  fin = fopen(train_file, "rb");
  if (fin == NULL) {
    printf("ERROR: training data file not found!\n");
    exit(1);
  }
  vocab_size = 0;
  AddWordToVocab((char *)"</s>");
  while (1) {
    ReadWord(word, fin);
    if (feof(fin)) break;

    // 忽略NEG这个单词，同时将neg_flag置为真，表示该行后面的单词都是负采样的了，不需要录入了 lw
    if (!strcmp(word, "NEG")) {
      neg_flag = true;
      continue;
    }
    // 遇到换行，就将neg_flag置为假，因为新的一行开始了 lw
    if (!strcmp(word, "</s>")) {
      neg_flag = false;
    }
    // 当neg_flag为真时，说明正处于NEG标志位的后面，则无需录入 lw
    if (neg_flag == true) {
      continue;
    }

    train_words++;

    if ((debug_mode > 1) && (train_words % 100000 == 0)) {
      printf("%lldK%c", train_words / 1000, 13);
      fflush(stdout);
    }

    i = SearchVocab(word);
    if (i == -1) {
      a = AddWordToVocab(word);
      vocab[a].cn = 1;
    } else vocab[i].cn++;
    if (vocab_size > vocab_hash_size * 0.7) ReduceVocab();
  }
  SortVocab();
  if (debug_mode > 0) {
    printf("Vocab size: %lld\n", vocab_size);
    printf("Words in train file: %lld\n", train_words);
  }

  file_size = ftell(fin);
  fclose(fin);
}

void SaveVocab() {
  long long i;
  FILE *fo = fopen(save_vocab_file, "wb");
  for (i = 0; i < vocab_size; i++) fprintf(fo, "%s %lld\n", vocab[i].word, vocab[i].cn);
  fclose(fo);
}

void ReadVocab() {
  long long a, i = 0;
  char c;
  char word[MAX_STRING];
  FILE *fin = fopen(read_vocab_file, "rb");
  if (fin == NULL) {
    printf("Vocabulary file not found\n");
    exit(1);
  }
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  vocab_size = 0;
  while (1) {
    ReadWord(word, fin);
    if (feof(fin)) break;
    // 忽略NEG lw
    if (!strcmp(word, "NEG")) continue;
    a = AddWordToVocab(word);
    fscanf(fin, "%lld%c", &vocab[a].cn, &c);
    i++;
  }
  SortVocab();
  if (debug_mode > 0) {
    printf("Vocab size: %lld\n", vocab_size);
    printf("Words in train file: %lld\n", train_words);
  }
  fin = fopen(train_file, "rb");
  if (fin == NULL) {
    printf("ERROR: training data file not found!\n");
    exit(1);
  }
  fseek(fin, 0, SEEK_END);
  file_size = ftell(fin);
  fclose(fin);
}

void InitNet() {
  long long a, b;
  unsigned long long next_random = 1;

#ifdef _MSC_VER
  syn0 = _aligned_malloc((long long)vocab_size * layer1_size * sizeof(real), 128);
#elif defined  linux
  a = posix_memalign((void **)&syn0, 128, (long long)vocab_size * layer1_size * sizeof(real));
#endif

  if (syn0 == NULL) {printf("Memory allocation failed\n"); exit(1);}
  if (hs) {
#ifdef _MSC_VER
    syn1 = _aligned_malloc((long long)vocab_size * layer1_size * sizeof(real), 128);
#elif defined  linux
    a = posix_memalign((void **)&syn1, 128, (long long)vocab_size * layer1_size * sizeof(real));
#endif
    if (syn1 == NULL) {printf("Memory allocation failed\n"); exit(1);}
    for (a = 0; a < vocab_size; a++) for (b = 0; b < layer1_size; b++)
        syn1[a * layer1_size + b] = 0;
  }
  if (negative>0) {
#ifdef _MSC_VER
    syn1neg = _aligned_malloc((long long)vocab_size * layer1_size * sizeof(real), 128);
#elif defined  linux
    a = posix_memalign((void **)&syn1neg, 128, (long long)vocab_size * layer1_size * sizeof(real));
#endif

    if (syn1neg == NULL) {printf("Memory allocation failed\n"); exit(1);}
    for (a = 0; a < vocab_size; a++) for (b = 0; b < layer1_size; b++)
        syn1neg[a * layer1_size + b] = 0;
  }
  for (a = 0; a < vocab_size; a++) for (b = 0; b < layer1_size; b++) {
      next_random = next_random * (unsigned long long)25214903917 + 11;
      syn0[a * layer1_size + b] = (((next_random & 0xFFFF) / (real)65536) - 0.5) / layer1_size;
    }
  CreateBinaryTree();
}

void taketime() {
  int a, b;
  for(a=0;a<10000000;a++)
  {
    b += a;
  }
}

void *TrainModelThread(void *id) {
  long long a, b, d, cw, word, last_word, sentence_length = 0, sentence_position = 0;
  // 句子中负采样的长度 lw
  long long sentence_neg_length = 0;
  long long word_count = 0, last_word_count = 0, sen[MAX_SENTENCE_LENGTH + 1];
  // sen_neg是每个句子的NEG后跟的负采样 lw
  long long sen_neg[MAX_SENTENCE_NEG_LENGTH + 1];
  // 几个标志位，具体含义自行体会吧，懒得写了 lw
  bool sentence_length_full_flag, sentence_neg_length_full_flag, neg_flag;
  long long l1, l2, c, target, label, local_iter = iter;
  unsigned long long next_random = (long long)id;
  real f, g;
  clock_t now;
  real *neu1 = (real *)calloc(layer1_size, sizeof(real));
  real *neu1e = (real *)calloc(layer1_size, sizeof(real));
  FILE *fi = fopen(train_file, "rb");
  fseek(fi, file_size / (long long)num_threads * (long long)id, SEEK_SET);
  while (1) {
    if (word_count - last_word_count > 10000) {
      word_count_actual += word_count - last_word_count;
      last_word_count = word_count;
      if ((debug_mode > 1)) {
        now=clock();
        printf("%cAlpha: %f  Progress: %.2f%%  Words/thread/sec: %.2fk  ", 13, alpha,
               word_count_actual / (real)(iter * train_words + 1) * 100,
               word_count_actual / ((real)(now - start + 1) / (real)CLOCKS_PER_SEC * 1000));
        fflush(stdout);
      }
      alpha = starting_alpha * (1 - word_count_actual / (real)(iter * train_words + 1));
      if (alpha < starting_alpha * 0.0001) alpha = starting_alpha * 0.0001;
    }
    if (sentence_length == 0) {
      // 句子开始读取时，所有标志位置为false lw
      sentence_length_full_flag = sentence_neg_length_full_flag = neg_flag = false;

      while (1) {
        word = ReadWordIndex(fi);
        if (feof(fi)) break;

        // 如果单词是NEG，则将负采样的标志位置为true lw
        if (word == -2) {
          neg_flag = true;
          //printf("--2");usleep(100000);
          continue;
        }
        if (word == 0) {
          // 防止文件被进程切分后第一句是在NEG后面 lw
          // 因为当一句话结束时，必然会遇到NEG标志位，则必然neg_flag为真，而如果neg_flag为假，说明这句话的NEG标志位没被切分进来 lw
          if (neg_flag == false) {
            word_count = 0;
            sentence_length = 0;
            sentence_neg_length = 0;
          }
          break;
        }
        if (word == -1) continue;

        // 当没有达到负采样位置且句子长度不超过最大长度时，累计单词数目 lw
        if (neg_flag == false && sentence_length_full_flag == false) {
          word_count++;
        }

        // 采集sen和sen_neg的句子，如果超出长度，则停止采集 lw
        if (neg_flag == false) {
          // The subsampling randomly discards frequent words while keeping the ranking same
          if (sample > 0) {
            real ran = (sqrt(vocab[word].cn / (sample * train_words)) + 1) * (sample * train_words) / vocab[word].cn;
            next_random = next_random * (unsigned long long)25214903917 + 11;
            if (ran < (next_random & 0xFFFF) / (real)65536) continue;
          }
          if (sentence_length_full_flag == false) {
            sen[sentence_length] = word;
            sentence_length++;
            if (sentence_length >= MAX_SENTENCE_LENGTH) {
              sentence_length_full_flag = true;
            }
          }
        }
        else if (word != -2) {
          if (sentence_neg_length_full_flag == false) {
            sen_neg[sentence_neg_length] = word;
            sentence_neg_length++;
            if (sentence_neg_length >= MAX_SENTENCE_NEG_LENGTH) {
              sentence_neg_length_full_flag = true;
            }
          }
        }
      }
      sentence_position = 0;
    }
    if (feof(fi) || (word_count > train_words / num_threads)) {
      word_count_actual += word_count - last_word_count;
      local_iter--;
      if (local_iter == 0) break;
      word_count = 0;
      last_word_count = 0;
      sentence_length = 0;
      // 句子的负采样的长度置为0 lw
      sentence_neg_length = 0;
      fseek(fi, file_size / (long long)num_threads * (long long)id, SEEK_SET);
      continue;
    }
    // 防止切分时导致的一些问题,则忽略该行 lw
    if (sentence_length == 0) {
      sentence_length = 0;
      sentence_neg_length = 0;
      continue;
    }
    word = sen[sentence_position];
    if (word == -1) continue;
    for (c = 0; c < layer1_size; c++) neu1[c] = 0;
    for (c = 0; c < layer1_size; c++) neu1e[c] = 0;
    next_random = next_random * (unsigned long long)25214903917 + 11;
    b = next_random % window;
    if (cbow) {  //train the cbow architecture
      // in -> hidden
      cw = 0;
      for (a = b; a < window * 2 + 1 - b; a++) if (a != window) {
          c = sentence_position - window + a;
          if (c < 0) continue;
          if (c >= sentence_length) continue;
          last_word = sen[c];
          if (last_word == -1) continue;
          for (c = 0; c < layer1_size; c++) neu1[c] += syn0[c + last_word * layer1_size];
          cw++;
        }
      if (cw) {
        for (c = 0; c < layer1_size; c++) neu1[c] /= cw;
        if (hs) for (d = 0; d < vocab[word].codelen; d++) {
            f = 0;
            l2 = vocab[word].point[d] * layer1_size;
            // Propagate hidden -> output
            for (c = 0; c < layer1_size; c++) f += neu1[c] * syn1[c + l2];
            if (f <= -MAX_EXP) continue;
            else if (f >= MAX_EXP) continue;
            else f = expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))];
            // 'g' is the gradient multiplied by the learning rate
            g = (1 - vocab[word].code[d] - f) * alpha;
            // Propagate errors output -> hidden
            for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1[c + l2];
            // Learn weights hidden -> output
            for (c = 0; c < layer1_size; c++) syn1[c + l2] += g * neu1[c];
          }
        // NEGATIVE SAMPLING
        if (negative > 0) for (d = 0; d < negative + 1; d++) {
            if (d == 0) {
              target = word;
              label = 1;
            } else {
              next_random = next_random * (unsigned long long)25214903917 + 11;
              // 从sen_neg中选择出负样本，不能单纯从这里选取，最多只选择一半 lw
              if (sentence_neg_length > 0 && ((next_random >> 16) % (MAX_SENTENCE_NEG_LENGTH*2)) < sentence_neg_length) {
                target = sen_neg[(next_random >> 16) % sentence_neg_length];
              } else {
                target = table[(next_random >> 16) % table_size];
                if (target == 0) target = next_random % (vocab_size - 1) + 1;
              }
              if (target == word) continue;
              label = 0;
            }
            l2 = target * layer1_size;
            f = 0;
            for (c = 0; c < layer1_size; c++) f += neu1[c] * syn1neg[c + l2];
            if (f > MAX_EXP) g = (label - 1) * alpha;
            else if (f < -MAX_EXP) g = (label - 0) * alpha;
            else g = (label - expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))]) * alpha;
            for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1neg[c + l2];
            for (c = 0; c < layer1_size; c++) syn1neg[c + l2] += g * neu1[c];
          }
        // hidden -> in
        for (a = b; a < window * 2 + 1 - b; a++) if (a != window) {
            c = sentence_position - window + a;
            if (c < 0) continue;
            if (c >= sentence_length) continue;
            last_word = sen[c];
            if (last_word == -1) continue;
            for (c = 0; c < layer1_size; c++) syn0[c + last_word * layer1_size] += neu1e[c];
          }
      }
    } else {  //train skip-gram
      for (a = b; a < window * 2 + 1 - b; a++) if (a != window) {
          c = sentence_position - window + a;
          if (c < 0) continue;
          if (c >= sentence_length) continue;
          last_word = sen[c];
          if (last_word == -1) continue;
          l1 = last_word * layer1_size;
          for (c = 0; c < layer1_size; c++) neu1e[c] = 0;
          // HIERARCHICAL SOFTMAX
          if (hs) for (d = 0; d < vocab[word].codelen; d++) {
              f = 0;
              l2 = vocab[word].point[d] * layer1_size;
              // Propagate hidden -> output
              for (c = 0; c < layer1_size; c++) f += syn0[c + l1] * syn1[c + l2];
              if (f <= -MAX_EXP) continue;
              else if (f >= MAX_EXP) continue;
              else f = expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))];
              // 'g' is the gradient multiplied by the learning rate
              g = (1 - vocab[word].code[d] - f) * alpha;
              // Propagate errors output -> hidden
              for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1[c + l2];
              // Learn weights hidden -> output
              for (c = 0; c < layer1_size; c++) syn1[c + l2] += g * syn0[c + l1];
            }
          // NEGATIVE SAMPLING
          if (negative > 0) for (d = 0; d < negative + 1; d++) {
              if (d == 0) {
                target = word;
                label = 1;
              } else {
                next_random = next_random * (unsigned long long)25214903917 + 11;
                // 从sen_neg中选择出负样本，不能单纯从这里选取，最多只选择一半 lw
                if (sentence_neg_length > 0 && ((next_random >> 16) % (MAX_SENTENCE_NEG_LENGTH*2)) < sentence_neg_length) {
                  target = sen_neg[(next_random >> 16) % sentence_neg_length];
                } else {
                  target = table[(next_random >> 16) % table_size];
                  if (target == 0) target = next_random % (vocab_size - 1) + 1;
                }
                if (target == word) continue;
                label = 0;
              }
              l2 = target * layer1_size;
              f = 0;
              for (c = 0; c < layer1_size; c++) f += syn0[c + l1] * syn1neg[c + l2];
              if (f > MAX_EXP) g = (label - 1) * alpha;
              else if (f < -MAX_EXP) g = (label - 0) * alpha;
              else g = (label - expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))]) * alpha;
              for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1neg[c + l2];
              for (c = 0; c < layer1_size; c++) syn1neg[c + l2] += g * syn0[c + l1];
            }
          // Learn weights input -> hidden
          for (c = 0; c < layer1_size; c++) syn0[c + l1] += neu1e[c];
        }
    }
    sentence_position++;
    if (sentence_position >= sentence_length) {
      sentence_length = 0;
      // 负采样句子长度也要置为0 lw
      sentence_neg_length = 0;
      continue;
    }
  }
  fclose(fi);
  free(neu1);
  free(neu1e);
#ifdef _MSC_VER
  _endthreadex(0);
#elif defined  linux
  pthread_exit(NULL);
#endif
}

#ifdef _MSC_VER
DWORD WINAPI TrainModelThread_win(LPVOID tid){
	TrainModelThread(tid);
	return 0;
}
#endif

void TrainModel() {
  long a, b, c, d;
  FILE *fo;
  printf("Starting training using file %s\n", train_file);
  starting_alpha = alpha;

  if (read_vocab_file[0] != 0) {
    printf("begin read vocab\n");
    ReadVocab();
    printf("finish read vocab\n");
  } else {
    printf("begin learn vocab from train file\n");
    LearnVocabFromTrainFile();
    printf("finish learn vocab from train file\n");
  }

  if (save_vocab_file[0] != 0) SaveVocab();
  if (output_file[0] == 0) return;

  printf("begin init net\n");
  InitNet();
  printf("finish init net\n");

  if (negative > 0) InitUnigramTable();
  start = clock();

  printf("begin training\n");
#ifdef _MSC_VER
  HANDLE *pt = (HANDLE *)malloc(num_threads * sizeof(HANDLE));
	for (int i = 0; i < num_threads; i++){
		pt[i] = (HANDLE)_beginthreadex(NULL, 0, TrainModelThread_win, (void *)i, 0, NULL);
	}
	WaitForMultipleObjects(num_threads, pt, TRUE, INFINITE);
	for (int i = 0; i < num_threads; i++){
		CloseHandle(pt[i]);
	}
	free(pt);
#elif defined  linux
  pthread_t *pt = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
  for (a = 0; a < num_threads; a++) pthread_create(&pt[a], NULL, TrainModelThread, (void *)a);
  for (a = 0; a < num_threads; a++) pthread_join(pt[a], NULL);
#endif
  printf("finish training\n");

  printf("begin output embedding vector to file\n");
  fo = fopen(output_file, "wb");
  if (classes == 0) {
    // Save the word vectors
    fprintf(fo, "%lld %lld\n", vocab_size, layer1_size);
    // 定义词向量的长度，用于归一化 lw
    double vector_length;
    for (a = 0; a < vocab_size; a++) {
      fprintf(fo, "%s ", vocab[a].word);

      // 词向量归一化 lw
      vector_length = 0;
      for (b = 0; b < layer1_size; b++) {
        vector_length += pow(syn0[a * layer1_size + b], 2);
      }
      if (vector_length > 0) {
        vector_length = sqrt(vector_length);
        for (b = 0; b < layer1_size; b++)
          syn0[a * layer1_size + b] /= vector_length;
      }

      if (binary) for (b = 0; b < layer1_size; b++) fwrite(&syn0[a * layer1_size + b], sizeof(real), 1, fo);
      else for (b = 0; b < layer1_size; b++) fprintf(fo, "%lf ", syn0[a * layer1_size + b]);
      fprintf(fo, "\n");
    }
  } else {
    // Run K-means on the word vectors
    int clcn = classes, iter = 10, closeid;
    int *centcn = (int *)malloc(classes * sizeof(int));
    int *cl = (int *)calloc(vocab_size, sizeof(int));
    real closev, x;
    real *cent = (real *)calloc(classes * layer1_size, sizeof(real));
    for (a = 0; a < vocab_size; a++) cl[a] = a % clcn;
    for (a = 0; a < iter; a++) {
      for (b = 0; b < clcn * layer1_size; b++) cent[b] = 0;
      for (b = 0; b < clcn; b++) centcn[b] = 1;
      for (c = 0; c < vocab_size; c++) {
        for (d = 0; d < layer1_size; d++) cent[layer1_size * cl[c] + d] += syn0[c * layer1_size + d];
        centcn[cl[c]]++;
      }
      for (b = 0; b < clcn; b++) {
        closev = 0;
        for (c = 0; c < layer1_size; c++) {
          cent[layer1_size * b + c] /= centcn[b];
          closev += cent[layer1_size * b + c] * cent[layer1_size * b + c];
        }
        closev = sqrt(closev);
        for (c = 0; c < layer1_size; c++) cent[layer1_size * b + c] /= closev;
      }
      for (c = 0; c < vocab_size; c++) {
        closev = -10;
        closeid = 0;
        for (d = 0; d < clcn; d++) {
          x = 0;
          for (b = 0; b < layer1_size; b++) x += cent[layer1_size * d + b] * syn0[c * layer1_size + b];
          if (x > closev) {
            closev = x;
            closeid = d;
          }
        }
        cl[c] = closeid;
      }
    }
    // Save the K-means classes
    for (a = 0; a < vocab_size; a++) fprintf(fo, "%s %d\n", vocab[a].word, cl[a]);
    free(centcn);
    free(cent);
    free(cl);
  }
  printf("finish output embedding vector to file\n");
  fclose(fo);
}

int ArgPos(char *str, int argc, char **argv) {
  int a;
  for (a = 1; a < argc; a++) if (!strcmp(str, argv[a])) {
      if (a == argc - 1) {
        printf("Argument missing for %s\n", str);
        exit(1);
      }
      return a;
    }
  return -1;
}

int main(int argc, char **argv) {
  int i;
  if (argc == 1) {
    printf("WORD VECTOR estimation toolkit v 0.1c\n\n");
    printf("Options:\n");
    printf("Parameters for training:\n");
    printf("\t-train <file>\n");
    printf("\t\tUse text data from <file> to train the model\n");
    printf("\t-output <file>\n");
    printf("\t\tUse <file> to save the resulting word vectors / word clusters\n");
    printf("\t-size <int>\n");
    printf("\t\tSet size of word vectors; default is 100\n");
    printf("\t-window <int>\n");
    printf("\t\tSet max skip length between words; default is 5\n");
    printf("\t-sample <float>\n");
    printf("\t\tSet threshold for occurrence of words. Those that appear with higher frequency in the training data\n");
    printf("\t\twill be randomly down-sampled; default is 1e-3, useful range is (0, 1e-5)\n");
    printf("\t-hs <int>\n");
    printf("\t\tUse Hierarchical Softmax; default is 0 (not used)\n");
    printf("\t-negative <int>\n");
    printf("\t\tNumber of negative examples; default is 5, common values are 3 - 10 (0 = not used)\n");
    printf("\t-threads <int>\n");
    printf("\t\tUse <int> threads (default 12)\n");
    printf("\t-iter <int>\n");
    printf("\t\tRun more training iterations (default 5)\n");
    printf("\t-min-count <int>\n");
    printf("\t\tThis will discard words that appear less than <int> times; default is 5\n");
    printf("\t-alpha <float>\n");
    printf("\t\tSet the starting learning rate; default is 0.025 for skip-gram and 0.05 for CBOW\n");
    printf("\t-classes <int>\n");
    printf("\t\tOutput word classes rather than word vectors; default number of classes is 0 (vectors are written)\n");
    printf("\t-debug <int>\n");
    printf("\t\tSet the debug mode (default = 2 = more info during training)\n");
    printf("\t-binary <int>\n");
    printf("\t\tSave the resulting vectors in binary moded; default is 0 (off)\n");
    printf("\t-save-vocab <file>\n");
    printf("\t\tThe vocabulary will be saved to <file>\n");
    printf("\t-read-vocab <file>\n");
    printf("\t\tThe vocabulary will be read from <file>, not constructed from the training data\n");
    printf("\t-cbow <int>\n");
    printf("\t\tUse the continuous bag of words model; default is 1 (use 0 for skip-gram model)\n");
    printf("\nExamples:\n");
    printf("./word2vec -train data.txt -output vec.txt -size 200 -window 5 -sample 1e-4 -negative 5 -hs 0 -binary 0 -cbow 1 -iter 3\n\n");
    return 0;
  }
  output_file[0] = 0;
  save_vocab_file[0] = 0;
  read_vocab_file[0] = 0;
  if ((i = ArgPos((char *)"-size", argc, argv)) > 0) layer1_size = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-train", argc, argv)) > 0) strcpy(train_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-save-vocab", argc, argv)) > 0) strcpy(save_vocab_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-read-vocab", argc, argv)) > 0) strcpy(read_vocab_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-debug", argc, argv)) > 0) debug_mode = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-binary", argc, argv)) > 0) binary = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-cbow", argc, argv)) > 0) cbow = atoi(argv[i + 1]);
  if (cbow) alpha = 0.05;
  if ((i = ArgPos((char *)"-alpha", argc, argv)) > 0) alpha = atof(argv[i + 1]);
  if ((i = ArgPos((char *)"-output", argc, argv)) > 0) strcpy(output_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-window", argc, argv)) > 0) window = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-sample", argc, argv)) > 0) sample = atof(argv[i + 1]);
  if ((i = ArgPos((char *)"-hs", argc, argv)) > 0) hs = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-negative", argc, argv)) > 0) negative = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-threads", argc, argv)) > 0) num_threads = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-iter", argc, argv)) > 0) iter = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-min-count", argc, argv)) > 0) min_count = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-classes", argc, argv)) > 0) classes = atoi(argv[i + 1]);
  vocab = (struct vocab_word *)calloc(vocab_max_size, sizeof(struct vocab_word));
  vocab_hash = (int *)calloc(vocab_hash_size, sizeof(int));
  expTable = (real *)malloc((EXP_TABLE_SIZE + 1) * sizeof(real));
  for (i = 0; i < EXP_TABLE_SIZE; i++) {
    expTable[i] = exp((i / (real)EXP_TABLE_SIZE * 2 - 1) * MAX_EXP); // Precompute the exp() table
    expTable[i] = expTable[i] / (expTable[i] + 1);                   // Precompute f(x) = x / (x + 1)
  }
  TrainModel();
  return 0;
}
