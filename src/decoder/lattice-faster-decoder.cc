// decoder/lattice-faster-decoder.cc

// Copyright 2009-2012  Microsoft Corporation  Mirko Hannemann
//           2013-2018  Johns Hopkins University (Author: Daniel Povey)
//                2014  Guoguo Chen
//                2018  Zhehuai Chen

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#include "decoder/lattice-faster-decoder.h"
#include "lat/lattice-functions.h"

namespace kaldi {

// instantiate this class once for each thing you have to decode.
template <typename FST, typename Token>
LatticeFasterDecoderTpl<FST, Token>::LatticeFasterDecoderTpl(
    const FST &fst,
    const LatticeFasterDecoderConfig &config):
    fst_(&fst), delete_fst_(false), config_(config) {
  config.Check();
  toks_.SetSize(1000);  // just so on the first frame we do something reasonable.
}


template <typename FST, typename Token>
LatticeFasterDecoderTpl<FST, Token>::LatticeFasterDecoderTpl(
    const LatticeFasterDecoderConfig &config, FST *fst):
    fst_(fst), delete_fst_(true), config_(config) {
  config.Check();
  toks_.SetSize(1000);  // just so on the first frame we do something reasonable.
}


template <typename FST, typename Token>
LatticeFasterDecoderTpl<FST, Token>::~LatticeFasterDecoderTpl() {
  DeleteElems(toks_.Clear());
  if (delete_fst_) delete fst_;
}

template <typename FST, typename Token>
void LatticeFasterDecoderTpl<FST, Token>::InitDecoding() {
  // clean up from last time:
  DeleteElems(toks_.Clear());
  frames_.DeleteAll();
  warned_ = false;
  decoding_finalized_ = false;
  final_costs_.clear();
  StateId start_state = fst_->Start();
  KALDI_ASSERT(start_state != fst::kNoStateId);
  frames_.Add();
  auto &start_tok = frames_.back().tokens.Add(0.0, 0.0, nullptr);
  toks_.Insert(start_state, &start_tok);
  ProcessNonemitting(config_.beam);
}

// Returns true if any kind of traceback is available (not necessarily from
// a final state).  It should only very rarely return false; this indicates
// an unusual search error.
template <typename FST, typename Token>
bool LatticeFasterDecoderTpl<FST, Token>::Decode(DecodableInterface *decodable) {
  InitDecoding();
  // We use 1-based indexing for frames in this decoder (if you view it in
  // terms of features), but note that the decodable object uses zero-based
  // numbering, which we have to correct for when we call it.
  AdvanceDecoding(decodable);
  FinalizeDecoding();

  // Returns true if we have any kind of traceback available (not necessarily
  // to the end state; query ReachedFinal() for that).
  return !frames_.empty() && !frames_.back().tokens.empty();
}


// Outputs an FST corresponding to the single best path through the lattice.
template <typename FST, typename Token>
bool LatticeFasterDecoderTpl<FST, Token>::GetBestPath(Lattice *olat,
                                       bool use_final_probs) const {
  Lattice raw_lat;
  GetRawLattice(&raw_lat, use_final_probs);
  ShortestPath(raw_lat, olat);
  return (olat->NumStates() != 0);
}


// Outputs an FST corresponding to the raw, state-level lattice
template <typename FST, typename Token>
bool LatticeFasterDecoderTpl<FST, Token>::GetRawLattice(
    Lattice *ofst,
    bool use_final_probs) const {
  typedef LatticeArc Arc;
  typedef Arc::StateId StateId;
  typedef Arc::Weight Weight;
  typedef Arc::Label Label;

  // Note: you can't use the old interface (Decode()) if you want to
  // get the lattice with use_final_probs = false.  You'd have to do
  // InitDecoding() and then AdvanceDecoding().
  if (decoding_finalized_ && !use_final_probs)
    KALDI_ERR << "You cannot call FinalizeDecoding() and then call "
              << "GetRawLattice() with use_final_probs == false";

  unordered_map<const Token*, BaseFloat> final_costs_local;

  const unordered_map<const Token*, BaseFloat> &final_costs =
      (decoding_finalized_ ? final_costs_ : final_costs_local);
  if (!decoding_finalized_ && use_final_probs)
    ComputeFinalCosts(&final_costs_local, NULL, NULL);

  ofst->DeleteStates();
  // num-frames plus one (since frames are one-based, and we have
  // an extra frame for the start-state).
  KALDI_ASSERT(frames_.size() > 1);
  const int32 bucket_count = frames_.token_count / 2 + 3;
  unordered_map<const Token*, StateId> tok_map(bucket_count);
  // First create all states.
  std::vector<const Token*> token_list;
  for (auto &frame : frames_) {
    if (frame.tokens.empty()) {
      KALDI_WARN << "GetRawLattice: no tokens active on frame " << frame.number
                 << ": not producing lattice.\n";
      return false;
    }
    TopSortTokens(frame.tokens, &token_list);
    for (size_t i = 0; i < token_list.size(); i++)
      if (token_list[i] != NULL)
        tok_map[token_list[i]] = ofst->AddState();
  }
  // The next statement sets the start state of the output FST.  Because we
  // topologically sorted the tokens, state zero must be the start-state.
  ofst->SetStart(0);

  KALDI_VLOG(4) << "init:" << frames_.token_count / 2 + 3 << " buckets:"
                << tok_map.bucket_count() << " load:" << tok_map.load_factor()
                << " max:" << tok_map.max_load_factor();
  // Now create all arcs.
  for (auto &frame : frames_) {
    for (auto &tok : frame.tokens) {
      StateId cur_state = tok_map[&tok];
      for (auto &l : tok.forward_links) {
        auto iter = tok_map.find(l.next_tok);
        KALDI_ASSERT(iter != tok_map.end());
        StateId nextstate = iter->second;
        BaseFloat cost_offset = 0.0;
        if (l.ilabel != 0) {  // emitting..
          cost_offset = frame.cost_offset;
        }
        Arc arc(l.ilabel, l.olabel,
                Weight(l.graph_cost, l.acoustic_cost - cost_offset),
                nextstate);
        ofst->AddArc(cur_state, arc);
      }
      if (frame == frames_.back()) {
        if (use_final_probs && !final_costs.empty()) {
          auto iter = final_costs.find(&tok);
          if (iter != final_costs.end())
            ofst->SetFinal(cur_state, LatticeWeight(iter->second, 0));
        } else {
          ofst->SetFinal(cur_state, LatticeWeight::One());
        }
      }
    }
  }
  return (ofst->NumStates() > 0);
}


// This function is now deprecated, since now we do determinization from outside
// the LatticeFasterDecoder class.  Outputs an FST corresponding to the
// lattice-determinized lattice (one path per word sequence).
template <typename FST, typename Token>
bool LatticeFasterDecoderTpl<FST, Token>::GetLattice(CompactLattice *ofst,
                                           bool use_final_probs) const {
  Lattice raw_fst;
  GetRawLattice(&raw_fst, use_final_probs);
  Invert(&raw_fst);  // make it so word labels are on the input.
  // (in phase where we get backward-costs).
  fst::ILabelCompare<LatticeArc> ilabel_comp;
  ArcSort(&raw_fst, ilabel_comp);  // sort on ilabel; makes
  // lattice-determinization more efficient.

  fst::DeterminizeLatticePrunedOptions lat_opts;
  lat_opts.max_mem = config_.det_opts.max_mem;

  DeterminizeLatticePruned(raw_fst, config_.lattice_beam, ofst, lat_opts);
  raw_fst.DeleteStates();  // Free memory-- raw_fst no longer needed.
  Connect(ofst);  // Remove unreachable states... there might be
  // a small number of these, in some cases.
  // Note: if something went wrong and the raw lattice was empty,
  // we should still get to this point in the code without warnings or failures.
  return (ofst->NumStates() != 0);
}

template <typename FST, typename Token>
void LatticeFasterDecoderTpl<FST, Token>::PossiblyResizeHash(size_t num_toks) {
  size_t new_sz = static_cast<size_t>(static_cast<BaseFloat>(num_toks)
                                      * config_.hash_ratio);
  if (new_sz > toks_.Size()) {
    toks_.SetSize(new_sz);
  }
}

/*
  A note on the definition of extra_cost.

  extra_cost is used in pruning tokens, to save memory.

  extra_cost can be thought of as a beta (backward) cost assuming
  we had set the betas on currently-active tokens to all be the negative
  of the alphas for those tokens.  (So all currently active tokens would
  be on (tied) best paths).

  We can use the extra_cost to accurately prune away tokens that we know will
  never appear in the lattice.  If the extra_cost is greater than the desired
  lattice beam, the token would provably never appear in the lattice, so we can
  prune away the token.

  (Note: we don't update all the extra_costs every time we update a frame; we
  only do it every 'config_.prune_interval' frames).
 */

// FindOrAddToken either locates a token in hash of toks_,
// or if necessary inserts a new, empty token (i.e. with no forward links)
// for the current frame.  [note: it's inserted if necessary into hash toks_
// and also into the list of tokens active on this frame (frame.tokens).
template <typename FST, typename Token>
inline typename LatticeFasterDecoderTpl<FST, Token>::Elem*
LatticeFasterDecoderTpl<FST, Token>::FindOrAddToken(
      StateId state, Frame &frame, BaseFloat tot_cost,
      Token *backpointer, bool *changed) {
  // Returns the Token pointer.  Sets "changed" (if non-NULL) to true
  // if the token was newly created or the cost changed.
  Elem *e_found = toks_.Insert(state, NULL);
  if (e_found->val == NULL) {  // no such token presently.
    const BaseFloat extra_cost = 0.0;
    // tokens on the currently final frame have zero extra_cost
    // as any of them could end up
    // on the winning path.
    auto &new_tok = frame.tokens.Add(tot_cost, extra_cost, backpointer);
    e_found->val = &new_tok;
    if (changed) *changed = true;
    return e_found;
  } else {
    Token *tok = e_found->val;  // There is an existing Token for this state.
    if (tok->tot_cost > tot_cost) {  // replace old token
      tok->tot_cost = tot_cost;
      // SetBackpointer() just does tok->backpointer = backpointer in
      // the case where Token == BackpointerToken, else nothing.
      tok->SetBackpointer(backpointer);
      // we don't allocate a new token, the old stays linked in frames_
      // we only replace the tot_cost
      // in the current frame, there are no forward links (and no extra_cost)
      // only in ProcessNonemitting we have to delete forward links
      // in case we visit a state for the second time
      // those forward links, that lead to this replaced token before:
      // they remain and will hopefully be pruned later (PruneForwardLinks...)
      if (changed) *changed = true;
    } else {
      if (changed) *changed = false;
    }
    return e_found;
  }
}

// prunes outgoing links for all tokens in frames_[frame].tokens
// it's called by PruneActiveTokens
// all links, that have link_extra_cost > lattice_beam are pruned
template <typename FST, typename Token>
void LatticeFasterDecoderTpl<FST, Token>::PruneForwardLinks(
    int32 frame_plus_one, bool *extra_costs_changed,
    bool *links_pruned, BaseFloat delta) {
  // delta is the amount by which the extra_costs must change
  // If delta is larger,  we'll tend to go back less far
  //    toward the beginning of the file.
  // extra_costs_changed is set to true if extra_cost was changed for any token
  // links_pruned is set to true if any link in any token was pruned

  *extra_costs_changed = false;
  *links_pruned = false;
  KALDI_ASSERT(frame_plus_one >= 0 && frame_plus_one < frames_.size());
  if (frames_[frame_plus_one].tokens.empty()) {  // empty list; should not happen.
    if (!warned_) {
      KALDI_WARN << "No tokens alive [doing pruning].. warning first "
          "time only for each utterance\n";
      warned_ = true;
    }
  }

  // We have to iterate until there is no more change, because the links
  // are not guaranteed to be in topological order.
  bool changed = true;  // difference new minus old extra cost >= delta ?
  while (changed) {
    changed = false;
    for (auto &tok : frames_[frame_plus_one].tokens) {
      // will recompute tok_extra_cost for tok.
      BaseFloat tok_extra_cost = std::numeric_limits<BaseFloat>::infinity();
      // tok_extra_cost is the best (min) of link_extra_cost of outgoing links
      for (auto link_iter = begin(tok.forward_links); link_iter != end(tok.forward_links);) {
        // See if we need to excise this link...
        auto &link = *link_iter;
        Token *next_tok = link.next_tok;
        BaseFloat link_extra_cost = next_tok->extra_cost +
            ((tok.tot_cost + link.acoustic_cost + link.graph_cost)
             - next_tok->tot_cost);  // difference in brackets is >= 0
        // link_exta_cost is the difference in score between the best paths
        // through link source state and through link destination state
        KALDI_ASSERT(link_extra_cost == link_extra_cost);  // check for NaN
        auto curr_link_iter = link_iter++;
        if (link_extra_cost > config_.lattice_beam) {  // excise link
          tok.forward_links.Delete(curr_link_iter);
          *links_pruned = true;
        } else {   // keep the link and update the tok_extra_cost if needed.
          if (link_extra_cost < 0.0) {  // this is just a precaution.
            if (link_extra_cost < -0.01)
              KALDI_WARN << "Negative extra_cost: " << link_extra_cost;
            link_extra_cost = 0.0;
          }
          if (link_extra_cost < tok_extra_cost)
            tok_extra_cost = link_extra_cost;
        }
      }  // for all outgoing links
      if (fabs(tok_extra_cost - tok.extra_cost) > delta)
        changed = true;   // difference new minus old is bigger than delta
      tok.extra_cost = tok_extra_cost;
      // will be +infinity or <= lattice_beam_.
      // infinity indicates, that no forward link survived pruning
    }  // for all Token on frames_[frame].tokens
    if (changed) *extra_costs_changed = true;

    // Note: it's theoretically possible that aggressive compiler
    // optimizations could cause an infinite loop here for small delta and
    // high-dynamic-range scores.
  } // while changed
}

// PruneForwardLinksFinal is a version of PruneForwardLinks that we call
// on the final frame.  If there are final tokens active, it uses
// the final-probs for pruning, otherwise it treats all tokens as final.
template <typename FST, typename Token>
void LatticeFasterDecoderTpl<FST, Token>::PruneForwardLinksFinal() {
  KALDI_ASSERT(!frames_.empty());

  if (frames_.back().tokens.empty())  // empty list; should not happen.
    KALDI_WARN << "No tokens alive at end of file";

  ComputeFinalCosts(&final_costs_, &final_relative_cost_, &final_best_cost_);
  decoding_finalized_ = true;
  // We call DeleteElems() as a nicety, not because it's really necessary;
  // otherwise there would be a time, after calling PruneTokensForFrame() on the
  // final frame, when toks_.GetList() or toks_.Clear() would contain pointers
  // to nonexistent tokens.
  DeleteElems(toks_.Clear());

  // Now go through tokens on this frame, pruning forward links...  may have to
  // iterate a few times until there is no more change, because the list is not
  // in topological order.  This is a modified version of the code in
  // PruneForwardLinks, but here we also take account of the final-probs.
  bool changed = true;
  BaseFloat delta = 1.0e-05;
  while (changed) {
    changed = false;
    for (auto &tok : frames_.back().tokens) {
      // will recompute tok_extra_cost.  It has a term in it that corresponds
      // to the "final-prob", so instead of initializing tok_extra_cost to infinity
      // below we set it to the difference between the (score+final_prob) of this token,
      // and the best such (score+final_prob).
      BaseFloat final_cost;
      if (final_costs_.empty()) {
        final_cost = 0.0;
      } else {
        auto iter = final_costs_.find(&tok);
        if (iter != final_costs_.end())
          final_cost = iter->second;
        else
          final_cost = std::numeric_limits<BaseFloat>::infinity();
      }
      BaseFloat tok_extra_cost = tok.tot_cost + final_cost - final_best_cost_;
      // tok_extra_cost will be a "min" over either directly being final, or
      // being indirectly final through other links, and the loop below may
      // decrease its value:
      for (auto link_iter = begin(tok.forward_links); link_iter != end(tok.forward_links); ) {
        auto &link = *link_iter;
        // See if we need to excise this link...
        Token *next_tok = link.next_tok;
        BaseFloat link_extra_cost = next_tok->extra_cost +
            ((tok.tot_cost + link.acoustic_cost + link.graph_cost)
             - next_tok->tot_cost);
        auto curr_link_iter = link_iter++;
        if (link_extra_cost > config_.lattice_beam) {  // excise link
          tok.forward_links.Delete(curr_link_iter);
        } else { // keep the link and update the tok_extra_cost if needed.
          if (link_extra_cost < 0.0) { // this is just a precaution.
            if (link_extra_cost < -0.01)
              KALDI_WARN << "Negative extra_cost: " << link_extra_cost;
            link_extra_cost = 0.0;
          }
          if (link_extra_cost < tok_extra_cost)
            tok_extra_cost = link_extra_cost;
        }
      }
      // prune away tokens worse than lattice_beam above best path.  This step
      // was not necessary in the non-final case because then, this case
      // showed up as having no forward links.  Here, the tok_extra_cost has
      // an extra component relating to the final-prob.
      if (tok_extra_cost > config_.lattice_beam)
        tok_extra_cost = std::numeric_limits<BaseFloat>::infinity();
      // to be pruned in PruneTokensForFrame

      if (!ApproxEqual(tok.extra_cost, tok_extra_cost, delta))
        changed = true;
      tok.extra_cost = tok_extra_cost; // will be +infinity or <= lattice_beam_.
    }
  } // while changed
}

template <typename FST, typename Token>
BaseFloat LatticeFasterDecoderTpl<FST, Token>::FinalRelativeCost() const {
  if (!decoding_finalized_) {
    BaseFloat relative_cost;
    ComputeFinalCosts(NULL, &relative_cost, NULL);
    return relative_cost;
  } else {
    // we're not allowed to call that function if FinalizeDecoding() has
    // been called; return a cached value.
    return final_relative_cost_;
  }
}


// Prune away any tokens on this frame that have no forward links.
// [we don't do this in PruneForwardLinks because it would give us
// a problem with dangling pointers].
// It's called by PruneActiveTokens if any forward links have been pruned
template <typename FST, typename Token>
void LatticeFasterDecoderTpl<FST, Token>::PruneTokensForFrame(int32 frame_plus_one) {
  KALDI_ASSERT(frame_plus_one >= 0 && frame_plus_one < frames_.size());
  auto &frame = frames_[frame_plus_one];
  if (frame.tokens.empty())
    KALDI_WARN << "No tokens alive [doing pruning]";
  for (auto tok_iter = begin(frame.tokens); tok_iter != end(frame.tokens);) {
    auto current_tok_iter = tok_iter++;
    if (current_tok_iter->extra_cost == std::numeric_limits<BaseFloat>::infinity()) {
      // token is unreachable from end of graph; (no forward links survived)
      // excise tok from list and delete tok.
      frame.tokens.Delete(current_tok_iter);
    }
  }
}

// Go backwards through still-alive tokens, pruning them, starting not from
// the current frame (where we want to keep all tokens) but from the frame before
// that.  We go backwards through the frames and stop when we reach a point
// where the delta-costs are not changing (and the delta controls when we consider
// a cost to have "not changed").
template <typename FST, typename Token>
void LatticeFasterDecoderTpl<FST, Token>::PruneActiveTokens(BaseFloat delta) {
  int32 cur_frame_plus_one = NumFramesDecoded();
  int32 num_toks_begin = frames_.token_count;
  // The index "f" below represents a "frame plus one", i.e. you'd have to subtract
  // one to get the corresponding index for the decodable object.
  for (int32 f = cur_frame_plus_one - 1; f >= 0; f--) {
    // Reason why we need to prune forward links in this situation:
    // (1) we have never pruned them (new Frame)
    // (2) we have not yet pruned the forward links to the next f,
    // after any of those tokens have changed their extra_cost.
    if (frames_[f].must_prune_forward_links) {
      bool extra_costs_changed = false, links_pruned = false;
      PruneForwardLinks(f, &extra_costs_changed, &links_pruned, delta);
      if (extra_costs_changed && f > 0) // any token has changed extra_cost
        frames_[f - 1].must_prune_forward_links = true;
      if (links_pruned) // any link was pruned
        frames_[f].must_prune_tokens = true;
      frames_[f].must_prune_forward_links = false; // job done
    }
    if (f+1 < cur_frame_plus_one &&      // except for last f (no forward links)
        frames_[f + 1].must_prune_tokens) {
      PruneTokensForFrame(f+1);
      frames_[f + 1].must_prune_tokens = false;
    }
  }
  KALDI_VLOG(4) << "PruneActiveTokens: pruned tokens from " << num_toks_begin
                << " to " << frames_.token_count;
}

template <typename FST, typename Token>
void LatticeFasterDecoderTpl<FST, Token>::ComputeFinalCosts(
    unordered_map<const Token*, BaseFloat> *final_costs,
    BaseFloat *final_relative_cost,
    BaseFloat *final_best_cost) const {
  KALDI_ASSERT(!decoding_finalized_);
  if (final_costs != NULL)
    final_costs->clear();
  const Elem *final_toks = toks_.GetList();
  BaseFloat infinity = std::numeric_limits<BaseFloat>::infinity();
  BaseFloat best_cost = infinity,
      best_cost_with_final = infinity;

  while (final_toks != NULL) {
    StateId state = final_toks->key;
    Token *tok = final_toks->val;
    const Elem *next = final_toks->tail;
    BaseFloat final_cost = fst_->Final(state).Value();
    BaseFloat cost = tok->tot_cost,
        cost_with_final = cost + final_cost;
    best_cost = std::min(cost, best_cost);
    best_cost_with_final = std::min(cost_with_final, best_cost_with_final);
    if (final_costs != NULL && final_cost != infinity)
      (*final_costs)[tok] = final_cost;
    final_toks = next;
  }
  if (final_relative_cost != NULL) {
    if (best_cost == infinity && best_cost_with_final == infinity) {
      // Likely this will only happen if there are no tokens surviving.
      // This seems the least bad way to handle it.
      *final_relative_cost = infinity;
    } else {
      *final_relative_cost = best_cost_with_final - best_cost;
    }
  }
  if (final_best_cost != NULL) {
    if (best_cost_with_final != infinity) { // final-state exists.
      *final_best_cost = best_cost_with_final;
    } else { // no final-state exists.
      *final_best_cost = best_cost;
    }
  }
}

template <typename FST, typename Token>
void LatticeFasterDecoderTpl<FST, Token>::AdvanceDecoding(DecodableInterface *decodable,
                                                int32 max_num_frames) {
  if (std::is_same<FST, fst::Fst<fst::StdArc> >::value) {
    // if the type 'FST' is the FST base-class, then see if the FST type of fst_
    // is actually VectorFst or ConstFst.  If so, call the AdvanceDecoding()
    // function after casting *this to the more specific type.
    if (fst_->Type() == "const") {
      LatticeFasterDecoderTpl<fst::ConstFst<fst::StdArc>, Token> *this_cast =
          reinterpret_cast<LatticeFasterDecoderTpl<fst::ConstFst<fst::StdArc>, Token>* >(this);
      this_cast->AdvanceDecoding(decodable, max_num_frames);
      return;
    } else if (fst_->Type() == "vector") {
      LatticeFasterDecoderTpl<fst::VectorFst<fst::StdArc>, Token> *this_cast =
          reinterpret_cast<LatticeFasterDecoderTpl<fst::VectorFst<fst::StdArc>, Token>* >(this);
      this_cast->AdvanceDecoding(decodable, max_num_frames);
      return;
    }
  }


  KALDI_ASSERT(!frames_.empty() && !decoding_finalized_ &&
               "You must call InitDecoding() before AdvanceDecoding");
  int32 num_frames_ready = decodable->NumFramesReady();
  // num_frames_ready must be >= num_frames_decoded, or else
  // the number of frames ready must have decreased (which doesn't
  // make sense) or the decodable object changed between calls
  // (which isn't allowed).
  KALDI_ASSERT(num_frames_ready >= NumFramesDecoded());
  int32 target_frames_decoded = num_frames_ready;
  if (max_num_frames >= 0)
    target_frames_decoded = std::min(target_frames_decoded,
                                     NumFramesDecoded() + max_num_frames);
  while (NumFramesDecoded() < target_frames_decoded) {
    if (NumFramesDecoded() % config_.prune_interval == 0) {
      PruneActiveTokens(config_.lattice_beam * config_.prune_scale);
    }
    BaseFloat cost_cutoff = ProcessEmitting(decodable);
    ProcessNonemitting(cost_cutoff);
  }
}

// FinalizeDecoding() is a version of PruneActiveTokens that we call
// (optionally) on the final frame.  Takes into account the final-prob of
// tokens.  This function used to be called PruneActiveTokensFinal().
template <typename FST, typename Token>
void LatticeFasterDecoderTpl<FST, Token>::FinalizeDecoding() {
  int32 final_frame_plus_one = NumFramesDecoded();
  int32 num_toks_begin = frames_.token_count;
  // PruneForwardLinksFinal() prunes final frame (with final-probs), and
  // sets decoding_finalized_.
  PruneForwardLinksFinal();
  for (int32 f = final_frame_plus_one - 1; f >= 0; f--) {
    bool b1, b2; // values not used.
    BaseFloat dontcare = 0.0; // delta of zero means we must always update
    PruneForwardLinks(f, &b1, &b2, dontcare);
    PruneTokensForFrame(f + 1);
  }
  PruneTokensForFrame(0);
  KALDI_VLOG(4) << "pruned tokens from " << num_toks_begin
                << " to " << frames_.token_count;
}

/// Gets the weight cutoff.  Also counts the active tokens.
template <typename FST, typename Token>
BaseFloat LatticeFasterDecoderTpl<FST, Token>::GetCutoff(Elem *list_head, size_t *tok_count,
                                          BaseFloat *adaptive_beam, Elem **best_elem) {
  BaseFloat best_weight = std::numeric_limits<BaseFloat>::infinity();
  // positive == high cost == bad.
  size_t count = 0;
  if (config_.max_active == std::numeric_limits<int32>::max() &&
      config_.min_active == 0) {
    for (Elem *e = list_head; e != NULL; e = e->tail, count++) {
      BaseFloat w = static_cast<BaseFloat>(e->val->tot_cost);
      if (w < best_weight) {
        best_weight = w;
        if (best_elem) *best_elem = e;
      }
    }
    if (tok_count != NULL) *tok_count = count;
    if (adaptive_beam != NULL) *adaptive_beam = config_.beam;
    return best_weight + config_.beam;
  } else {
    std::vector<BaseFloat> tmp_array;
    for (Elem *e = list_head; e != NULL; e = e->tail, count++) {
      BaseFloat w = e->val->tot_cost;
      tmp_array.push_back(w);
      if (w < best_weight) {
        best_weight = w;
        if (best_elem) *best_elem = e;
      }
    }
    if (tok_count != NULL) *tok_count = count;

    BaseFloat beam_cutoff = best_weight + config_.beam,
        min_active_cutoff = std::numeric_limits<BaseFloat>::infinity(),
        max_active_cutoff = std::numeric_limits<BaseFloat>::infinity();

    KALDI_VLOG(6) << "Number of tokens active on frame " << NumFramesDecoded()
                  << " is " << tmp_array.size();

    if (tmp_array.size() > static_cast<size_t>(config_.max_active)) {
      std::nth_element(tmp_array.begin(),
                       tmp_array.begin() + config_.max_active,
                       tmp_array.end());
      max_active_cutoff = tmp_array[config_.max_active];
    }
    if (max_active_cutoff < beam_cutoff) { // max_active is tighter than beam.
      if (adaptive_beam)
        *adaptive_beam = max_active_cutoff - best_weight + config_.beam_delta;
      return max_active_cutoff;
    }
    if (tmp_array.size() > static_cast<size_t>(config_.min_active)) {
      if (config_.min_active == 0) min_active_cutoff = best_weight;
      else {
        std::nth_element(tmp_array.begin(),
                         tmp_array.begin() + config_.min_active,
                         tmp_array.size() > static_cast<size_t>(config_.max_active) ?
                         tmp_array.begin() + config_.max_active :
                         tmp_array.end());
        min_active_cutoff = tmp_array[config_.min_active];
      }
    }
    if (min_active_cutoff > beam_cutoff) { // min_active is looser than beam.
      if (adaptive_beam)
        *adaptive_beam = min_active_cutoff - best_weight + config_.beam_delta;
      return min_active_cutoff;
    } else {
      *adaptive_beam = config_.beam;
      return beam_cutoff;
    }
  }
}

template <typename FST, typename Token>
BaseFloat LatticeFasterDecoderTpl<FST, Token>::ProcessEmitting(
    DecodableInterface *decodable) {
  KALDI_ASSERT(!frames_.empty());
  auto &last_frame = frames_.back();
  frames_.Add();

  Elem *final_toks = toks_.Clear(); // analogous to swapping prev_toks_ / cur_toks_
                                   // in simple-decoder.h.   Removes the Elems from
                                   // being indexed in the hash in toks_.
  Elem *best_elem = NULL;
  BaseFloat adaptive_beam;
  size_t tok_cnt;
  BaseFloat cur_cutoff = GetCutoff(final_toks, &tok_cnt, &adaptive_beam, &best_elem);
  KALDI_VLOG(6) << "Adaptive beam on frame " << NumFramesDecoded() << " is "
                << adaptive_beam;

  PossiblyResizeHash(tok_cnt);  // This makes sure the hash is always big enough.

  BaseFloat next_cutoff = std::numeric_limits<BaseFloat>::infinity();
  // pruning "online" before having seen all tokens

  BaseFloat cost_offset = 0.0; // Used to keep probabilities in a good
                               // dynamic range.


  // First process the best token to get a hopefully
  // reasonably tight bound on the next cutoff.  The only
  // products of the next block are "next_cutoff" and "cost_offset".
  if (best_elem) {
    StateId state = best_elem->key;
    Token *tok = best_elem->val;
    cost_offset = - tok->tot_cost;
    for (fst::ArcIterator<FST> aiter(*fst_, state);
         !aiter.Done();
         aiter.Next()) {
      const Arc &arc = aiter.Value();
      if (arc.ilabel != 0) {  // propagate..
        BaseFloat new_weight = arc.weight.Value() + cost_offset -
            decodable->LogLikelihood(last_frame.number, arc.ilabel) + tok->tot_cost;
        if (new_weight + adaptive_beam < next_cutoff)
          next_cutoff = new_weight + adaptive_beam;
      }
    }
  }

  // Store the offset on the acoustic likelihoods that we're applying.
  last_frame.cost_offset = cost_offset;

  // the tokens are now owned here, in final_toks, and the hash is empty.
  // 'owned' is a complex thing here; the point is we need to call DeleteElem
  // on each elem 'e' to let toks_ know we're done with them.
  for (Elem *e = final_toks, *e_tail; e != NULL; e = e_tail) {
    // loop this way because we delete "e" as we go.
    StateId state = e->key;
    Token *tok = e->val;
    if (tok->tot_cost <= cur_cutoff) {
      for (fst::ArcIterator<FST> aiter(*fst_, state);
           !aiter.Done();
           aiter.Next()) {
        const Arc &arc = aiter.Value();
        if (arc.ilabel != 0) {  // propagate..
          BaseFloat ac_cost = cost_offset -
              decodable->LogLikelihood(last_frame.number, arc.ilabel),
              graph_cost = arc.weight.Value(),
              cur_cost = tok->tot_cost,
              tot_cost = cur_cost + ac_cost + graph_cost;
          if (tot_cost >= next_cutoff) continue;
          else if (tot_cost + adaptive_beam < next_cutoff)
            next_cutoff = tot_cost + adaptive_beam; // prune by best current token
          // Note: the frame indexes into frames_ are one-based,
          // hence the + 1.
          Elem *e_next = FindOrAddToken(arc.nextstate,
                                        frames_.back(), tot_cost, tok, NULL);
          // NULL: no change indicator needed

          tok->forward_links.Add(e_next->val, arc.ilabel, arc.olabel, graph_cost, ac_cost);
        }
      } // for all arcs
    }
    e_tail = e->tail;
    toks_.Delete(e); // delete Elem
  }
  return next_cutoff;
}

template <typename FST, typename Token>
void LatticeFasterDecoderTpl<FST, Token>::ProcessNonemitting(BaseFloat cutoff) {
  KALDI_ASSERT(!frames_.empty());
  int32 frame = frames_.size() - 2;
  // Note: "frame" is the time-index we just processed, or -1 if
  // we are processing the nonemitting transitions before the
  // first frame (called from InitDecoding()).

  // Processes nonemitting arcs for one frame.  Propagates within toks_.
  // Note-- this queue structure is not very optimal as
  // it may cause us to process states unnecessarily (e.g. more than once),
  // but in the baseline code, turning this vector into a set to fix this
  // problem did not improve overall speed.

  std::vector<const Elem* > queue;

  if (toks_.GetList() == NULL) {
    if (!warned_) {
      KALDI_WARN << "Error, no surviving tokens: frame is " << frame;
      warned_ = true;
    }
  }

  for (const Elem *e = toks_.GetList(); e != NULL;  e = e->tail) {
    StateId state = e->key;
    if (fst_->NumInputEpsilons(state) != 0)
      queue.push_back(e);
  }

  while (!queue.empty()) {
    const Elem *e = queue.back();
    queue.pop_back();

    StateId state = e->key;
    Token *tok = e->val;  // would segfault if e is a NULL pointer but this can't happen.
    BaseFloat cur_cost = tok->tot_cost;
    if (cur_cost >= cutoff) // Don't bother processing successors.
      continue;
    // If "tok" has any existing forward links, delete them,
    // because we're about to regenerate them.  This is a kind
    // of non-optimality (remember, this is the simple decoder),
    // but since most states are emitting it's not a huge issue.
    tok->forward_links.DeleteAll();
    for (fst::ArcIterator<FST> aiter(*fst_, state);
         !aiter.Done();
         aiter.Next()) {
      const Arc &arc = aiter.Value();
      if (arc.ilabel == 0) {  // propagate nonemitting only...
        BaseFloat graph_cost = arc.weight.Value(),
            tot_cost = cur_cost + graph_cost;
        if (tot_cost < cutoff) {
          bool changed;

          Elem *e_new = FindOrAddToken(arc.nextstate, frames_.back(), tot_cost,
                                          tok, &changed);

          tok->forward_links.Add(e_new->val, 0, arc.olabel, graph_cost, 0);

          // "changed" tells us whether the new token has a different
          // cost from before, or is new [if so, add into queue].
          if (changed && fst_->NumInputEpsilons(arc.nextstate) != 0)
            queue.push_back(e_new);
        }
      }
    } // for all arcs
  } // while queue not empty
}


template <typename FST, typename Token>
void LatticeFasterDecoderTpl<FST, Token>::DeleteElems(Elem *list) {
  for (Elem *e = list, *e_tail; e != NULL; e = e_tail) {
    e_tail = e->tail;
    toks_.Delete(e);
  }
}

// static
template <typename FST, typename Token>
void LatticeFasterDecoderTpl<FST, Token>::TopSortTokens(
    const decoder::TokenList<Token> &tokens, std::vector<const Token*> *topsorted_list) {
  unordered_map<const Token*, int32> token2pos;
  int32 num_toks = tokens.size();
  int32 cur_pos = 0;
  // We assign the tokens numbers num_toks - 1, ... , 2, 1, 0.
  // This is likely to be in closer to topological order than
  // if we had given them ascending order, because of the way
  // new tokens are put at the front of the list.
  for (auto &tok : tokens)
    token2pos[&tok] = num_toks - ++cur_pos;

  unordered_set<const Token*> reprocess;

  for (auto iter = token2pos.begin(); iter != token2pos.end(); ++iter) {
    const Token *tok = iter->first;
    int32 pos = iter->second;
    for (auto &link : tok->forward_links) {
      if (link.ilabel == 0) {
        // We only need to consider epsilon links, since non-epsilon links
        // transition between frames and this function only needs to sort a list
        // of tokens from a single frame.
        auto following_iter = token2pos.find(link.next_tok);
        if (following_iter != token2pos.end()) { // another token on this frame,
                                                 // so must consider it.
          int32 next_pos = following_iter->second;
          if (next_pos < pos) { // reassign the position of the next Token.
            following_iter->second = cur_pos++;
            reprocess.insert(link.next_tok);
          }
        }
      }
    }
    // In case we had previously assigned this token to be reprocessed, we can
    // erase it from that set because it's "happy now" (we just processed it).
    reprocess.erase(tok);
  }

  size_t max_loop = 1000000, loop_count; // max_loop is to detect epsilon cycles.
  for (loop_count = 0; !reprocess.empty() && loop_count < max_loop; ++loop_count) {
    std::vector<const Token*> reprocess_vec;
    for (auto iter = reprocess.begin(); iter != reprocess.end(); ++iter)
      reprocess_vec.push_back(*iter);
    reprocess.clear();
    for (auto iter = reprocess_vec.begin(); iter != reprocess_vec.end(); ++iter) {
      const Token *tok = *iter;
      int32 pos = token2pos[tok];
      // Repeat the processing we did above (for comments, see above).
      for (auto &link : tok->forward_links) {
        if (link.ilabel == 0) {
          auto following_iter = token2pos.find(link.next_tok);
          if (following_iter != token2pos.end()) {
            int32 next_pos = following_iter->second;
            if (next_pos < pos) {
              following_iter->second = cur_pos++;
              reprocess.insert(link.next_tok);
            }
          }
        }
      }
    }
  }
  KALDI_ASSERT(loop_count < max_loop && "Epsilon loops exist in your decoding "
               "graph (this is not allowed!)");

  topsorted_list->clear();
  topsorted_list->resize(cur_pos, NULL);  // create a list with NULLs in between.
  for (auto iter = token2pos.begin(); iter != token2pos.end(); ++iter)
    (*topsorted_list)[iter->second] = iter->first;
}

// Instantiate the template for the combination of token types and FST types
// that we'll need.
template class LatticeFasterDecoderTpl<fst::Fst<fst::StdArc>, decoder::StdToken>;
template class LatticeFasterDecoderTpl<fst::VectorFst<fst::StdArc>, decoder::StdToken >;
template class LatticeFasterDecoderTpl<fst::ConstFst<fst::StdArc>, decoder::StdToken >;

template class LatticeFasterDecoderTpl<fst::ConstGrammarFst, decoder::StdToken>;
template class LatticeFasterDecoderTpl<fst::VectorGrammarFst, decoder::StdToken>;

template class LatticeFasterDecoderTpl<fst::Fst<fst::StdArc> , decoder::BackpointerToken>;
template class LatticeFasterDecoderTpl<fst::VectorFst<fst::StdArc>, decoder::BackpointerToken >;
template class LatticeFasterDecoderTpl<fst::ConstFst<fst::StdArc>, decoder::BackpointerToken >;
template class LatticeFasterDecoderTpl<fst::ConstGrammarFst, decoder::BackpointerToken>;
template class LatticeFasterDecoderTpl<fst::VectorGrammarFst, decoder::BackpointerToken>;


} // end namespace kaldi.
