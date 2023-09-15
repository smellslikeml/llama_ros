// MIT License

// Copyright (c) 2023  Miguel Ángel González Santamarta

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <cassert>
#include <cmath>
#include <thread>

#include "common.h"
#include "llama_ros/llama.hpp"

using namespace llama_ros;

Llama::Llama(const gpt_params &params, bool debug) : params(params) {

  this->debug = debug;

  // load the model
  llama_backend_init(this->params.numa);
  std::tie(this->model, this->ctx) = llama_init_from_gpt_params(this->params);

  // show system information
  fprintf(stderr, "System_info: n_threads = %d / %d | %s\n",
          this->params.n_threads, std::thread::hardware_concurrency(),
          llama_print_system_info());

  // prefix & suffix
  this->inp_pfx = this->tokenize(this->params.input_prefix, true);
  this->inp_sfx = this->tokenize(this->params.input_suffix, false);
  this->inp_stop = this->tokenize(this->params.antiprompt.at(0), false);

  // number of tokens to keep when resetting context
  if (this->params.n_keep == -1) {
    this->params.n_keep = (int)this->prompt_tokens.size();
  }

  // TODO: replace with ring-buffer
  this->last_n_tokens = std::vector<llama_token>(this->get_n_ctx());
  std::fill(this->last_n_tokens.begin(), this->last_n_tokens.end(), 0);

  this->is_antiprompt = false;
  this->canceled = false;
  this->n_past = 0;
  this->n_remain = this->params.n_predict;
  this->n_consumed = 0;

  // show info
  fprintf(stderr,
          "Generate: n_ctx = %d, n_batch = %d, n_predict = %d, n_keep = %d\n",
          this->get_n_ctx(), this->params.n_batch, this->params.n_predict,
          this->params.n_keep);

  // do one empty run to warm up the model
  {
    const std::vector<llama_token> tmp = {
        llama_token_bos(this->ctx),
    };
    llama_eval(this->ctx, tmp.data(), tmp.size(), 0, this->params.n_threads);
  }
}

Llama::~Llama() {
  llama_free(this->ctx);
  llama_free_model(this->model);
  llama_backend_free();
}

std::vector<llama_token> Llama::tokenize(const std::string &text,
                                         bool add_bos) {
  return llama_tokenize(this->ctx, text, add_bos);
}

std::string Llama::detokenize(const std::vector<llama_token> &tokens) {
  return llama_detokenize_bpe(this->ctx, tokens);
}

void Llama::reset() {
  this->last_n_tokens = std::vector<llama_token>(this->get_n_ctx());
  std::fill(this->last_n_tokens.begin(), this->last_n_tokens.end(), 0);

  this->is_antiprompt = false;
  this->canceled = false;
  this->n_past = 0;
  this->n_remain = this->params.n_predict;
  this->n_consumed = 0;

  this->prompt_tokens.clear();
  this->batch_tokens.clear();
}

void Llama::cancel() { this->canceled = true; }

std::vector<float> Llama::generate_embeddings(const std::string &input_prompt) {

  if (!this->is_embedding()) {
    fprintf(stderr,
            "Llama must be created with embedding=true to create embeddings\n");
    return {};
  }

  std::string prompt(input_prompt);
  auto tokens = this->tokenize(prompt, true);
  int n_past = 0;

  for (size_t i = 0; i < tokens.size(); i += this->params.n_batch) {

    int n_eval = (int)tokens.size() - i;
    if (n_eval > this->params.n_batch) {
      n_eval = this->params.n_batch;
    }

    if (llama_eval(this->ctx, &tokens[i], n_eval, n_past,
                   this->params.n_threads)) {
      fprintf(stderr, "Failed to eval\n");
    }
    n_past += n_eval;
  }

  const int n_embd = llama_n_embd(this->ctx);
  const auto embeddings = llama_get_embeddings(this->ctx);
  std::vector<float> embeddings_list;

  for (int i = 0; i < n_embd; i++) {
    embeddings_list.push_back(embeddings[i]);
  }

  return embeddings_list;
}

std::vector<completion_output>
Llama::generate_response(const std::string &input_prompt, bool add_pfx_sfx,
                         GenerateResponseCallback callback) {

  this->canceled = false;
  bool input_noecho = true;

  bool stopping = false;
  completion_output completion_result;
  std::vector<completion_output> response;
  std::vector<completion_output> completion_result_list;

  std::string prompt(input_prompt);
  std::vector<llama_token> line_inp;

  if (prompt.size() <= 0) {
    return {};
  }

  if (!this->prompt_tokens.size() && !add_pfx_sfx) {
    line_inp = this->tokenize(prompt, true);
  } else {
    line_inp = this->tokenize(prompt, false);
  }

  int prompt_size = this->prompt_tokens.size() + line_inp.size();
  if (add_pfx_sfx && this->params.input_prefix.size()) {
    prompt_size += this->inp_pfx.size() + this->inp_sfx.size();
  }

  if (prompt_size > this->get_n_ctx() - 4) {
    fprintf(stderr, "Prompt is too long (%d tokens, max %d)\n", prompt_size,
            this->get_n_ctx() - 4);
  }

  // insert prefix
  if (add_pfx_sfx && this->params.input_prefix.size() && !this->is_antiprompt) {
    this->prompt_tokens.insert(this->prompt_tokens.end(), this->inp_pfx.begin(),
                               this->inp_pfx.end());
  }

  this->prompt_tokens.insert(this->prompt_tokens.end(), line_inp.begin(),
                             line_inp.end());

  // insert suffix
  if (add_pfx_sfx && this->params.input_suffix.size()) {
    this->prompt_tokens.insert(this->prompt_tokens.end(), this->inp_sfx.begin(),
                               this->inp_sfx.end());
  }

  this->n_remain -= line_inp.size();

  // show sampling info
  fprintf(stderr,
          "Sampling: temp = %f, "
          "top_k = %d, "
          "top_p = %f, "
          "repeat_last_n = %i, "
          "repeat_penalty = %f\n",
          params.temp, params.top_k, params.top_p, params.repeat_last_n,
          params.repeat_penalty);

  // load grammar
  this->grammar = this->load_grammar(params.grammar);

  if (this->grammar != NULL) {
    auto it = params.logit_bias.find(llama_token_eos(this->ctx));

    if (it != params.logit_bias.end() && it->second == -INFINITY) {
      fprintf(stderr, "warning: EOS token is disabled, which will cause most "
                      "grammars to fail\n");
    }
  }

  fprintf(stderr, "Starting Response Generation\n");

  // generation loop
  while (this->n_remain != 0) {

    this->eval();

    if ((int)this->prompt_tokens.size() <= this->n_consumed) {

      // check if stop appears at the end of the output
      std::string last_output = this->detokenize(this->last_n_tokens);
      this->is_antiprompt = false;

      // when not currently processing queued
      // inputs check if we should end
      if (this->params.antiprompt.at(0).size() &&
          last_output.find(
              this->params.antiprompt.at(0).c_str(),
              last_output.length() - this->params.antiprompt.at(0).length(),
              this->params.antiprompt.at(0).length()) != std::string::npos) {
        this->is_antiprompt = true;
        break;
      }

      // sample next token
      completion_result = this->sample();
      completion_result_list.push_back(completion_result);

      this->batch_tokens.push_back(completion_result.token);
      this->last_n_tokens.erase(this->last_n_tokens.begin());
      this->last_n_tokens.push_back(completion_result.token);

      // echo this to console
      input_noecho = false;

      // decrement remaining sampling budget
      --this->n_remain;
    }

    if (this->batch_tokens.back() == llama_token_eos(this->ctx)) {
      break;
    }

    if (this->canceled) {
      fprintf(stderr, "Canceling llama.cpp\n");
      break;
    }

    // check if new tokens contains the stop sequence
    if (completion_result_list.size() <= this->inp_stop.size() &&
        this->params.antiprompt.at(0).size()) {

      stopping = true;

      for (size_t i = 0; i < completion_result_list.size(); i++) {
        if (completion_result_list.at(i).token != this->inp_stop.at(i)) {
          stopping = false;
          break;
        }
      }

      if (stopping && completion_result_list.size() == this->inp_stop.size()) {
        break;
      }

    } else {
      stopping = false;
    }

    // send text
    if (!input_noecho) {
      if (!stopping) {
        for (auto completion_ele : completion_result_list) {
          if (callback != nullptr) {
            callback(completion_ele);
          }
          response.push_back(completion_ele);
        }
        completion_result_list.clear();
      }
    }

    // respect the maximum number of tokens
    if (this->n_remain <= 0 && this->params.n_predict != -1) {
      this->n_remain = this->params.n_predict;
      break;
    }
  }

  fprintf(stderr, "Finish Response Generation\n");

  if (this->grammar != NULL) {
    llama_grammar_free(this->grammar);
    this->grammar = NULL;
  }

  return response;
}

void Llama::eval() {

  while (((int)this->prompt_tokens.size() > this->n_consumed) &&
         ((int)this->batch_tokens.size() < this->params.n_batch)) {

    this->batch_tokens.push_back(this->prompt_tokens[this->n_consumed]);
    this->last_n_tokens.erase(this->last_n_tokens.begin());
    this->last_n_tokens.push_back(this->prompt_tokens[this->n_consumed]);
    ++this->n_consumed;
  }

  // predict
  if (this->batch_tokens.size() > 0) {

    // infinite text generation via context swapping
    // if we run out of context:
    // - take the n_keep first tokens from the original prompt (via n_past)
    // - take half of the last (n_ctx - n_keep) tokens and recompute the
    // logits in a batch
    if (this->n_past + (int)this->batch_tokens.size() > this->get_n_ctx()) {

      const int n_left = this->n_past - this->params.n_keep;
      this->n_past = this->params.n_keep;

      // insert n_left/2 tokens at the start of batch_tokens
      // from last_n_tokens
      this->batch_tokens.insert(
          this->batch_tokens.begin(),
          this->last_n_tokens.begin() + this->get_n_ctx() - n_left / 2 -
              this->batch_tokens.size(),
          this->last_n_tokens.end() - this->batch_tokens.size());
    }

    // evaluate tokens in batches
    // batch_tokens is typically prepared beforehand to fit within a batch
    // but not always
    for (size_t i = 0; i < this->batch_tokens.size();
         i += this->params.n_batch) {

      int n_eval = (int)this->batch_tokens.size() - i;
      if (n_eval > this->params.n_batch) {
        n_eval = this->params.n_batch;
      }

      if (this->debug) {
        spinner.spin("EVALUATING " + std::to_string(n_eval) + " TOKENS");
      }

      if (llama_eval(this->ctx, &this->batch_tokens[i], n_eval, this->n_past,
                     this->params.n_threads)) {
        fprintf(stderr, "Failed to eval\n");
      }
      this->n_past += n_eval;
    }

    this->batch_tokens.clear();
  }
}

completion_output Llama::sample() {

  // init token
  auto logits = llama_get_logits(this->ctx);
  auto n_vocab = llama_n_vocab(this->ctx);

  // apply logit_bias
  for (auto it = params.logit_bias.begin(); it != params.logit_bias.end();
       it++) {
    logits[it->first] += it->second;
  }

  // candidates
  std::vector<llama_token_data> candidates;
  candidates.reserve(n_vocab);

  // sample token
  const llama_token id =
      llama_sample_token(this->ctx, NULL, this->grammar, this->params,
                         this->last_n_tokens, candidates);

  // create output
  completion_output result;
  result.token = id;

  for (size_t i = 0; i < std::min(candidates.size(), (size_t)params.n_probs);
       ++i) {
    result.probs.push_back({candidates[i].id, candidates[i].p});
  }

  return result;
}

llama_grammar *Llama::load_grammar(const std::string &grammar_text) {

  if (!grammar_text.empty()) {

    this->parsed_grammar = grammar_parser::parse(grammar_text.c_str());

    // will be empty (default) if there are parse errors
    if (parsed_grammar.rules.empty()) {
      return NULL;
    }

    if (this->debug) {
      fprintf(stderr, "\nGRAMMAR:\n");
      grammar_parser::print_grammar(stderr, parsed_grammar);
      fprintf(stderr, "\n");
    }

    std::vector<const llama_grammar_element *> grammar_rules(
        parsed_grammar.c_rules());
    return llama_grammar_init(grammar_rules.data(), grammar_rules.size(),
                              parsed_grammar.symbol_ids.at("root"));
  }

  return NULL;
}
