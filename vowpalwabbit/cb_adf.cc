/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
 */
#include <float.h>
#include <errno.h>

#include "reductions.h"
#include "v_hashmap.h"
#include "label_dictionary.h"
#include "vw.h"
#include "cb_algs.h"

using namespace std;
using namespace LEARNER;

#define CB_TYPE_DR 1
#define CB_TYPE_IPS 2

struct cb_adf {
  v_array<example*> ec_seq;

  size_t cb_type;
  bool need_to_clear;
  vw* all;
  LEARNER::base_learner* scorer;
  CB::cb_class* known_cost;
  v_array<CB::label> cb_labels;

  v_array<COST_SENSITIVE::label> cs_labels;

  base_learner* base;
};

namespace CB_ADF {

  void gen_cs_example_ips(v_array<example*> examples, v_array<COST_SENSITIVE::label>& cs_labels)
  {
    if (cs_labels.size() < examples.size()) {
      cs_labels.resize(examples.size(), true);
      cs_labels.end = cs_labels.end_array;
    }
    for (size_t i = 0; i < examples.size(); i++)
      {
	CB::label ld = examples[i]->l.cb;
	
	COST_SENSITIVE::wclass wc;
	wc.class_index = 0;
	if ( ld.costs.size() == 1 && ld.costs[0].cost != FLT_MAX)  
	  wc.x = ld.costs[0].cost / ld.costs[0].probability;
	else 
	  wc.x = 0.f;
	cs_labels[i].costs.erase();
	cs_labels[i].costs.push_back(wc);
      }
    cs_labels[examples.size()-1].costs[0].x = FLT_MAX; //trigger end of multiline example.

    if (examples[0]->l.cb.costs.size() > 0 && examples[0]->l.cb.costs[0].probability == -1.f)//take care of shared examples
      {
	cs_labels[0].costs[0].class_index = 0;
	cs_labels[0].costs[0].x = -1.f;
      }
  }
  
template <bool is_learn>
void gen_cs_label(cb_adf& c, example& ec, v_array<COST_SENSITIVE::label> array, uint32_t label)
{
	COST_SENSITIVE::wclass wc;
	wc.wap_value = 0.;

	//get cost prediction for this label
	// num_actions should be 1 effectively.
	// my get_cost_pred function will use 1 for 'index-1+base'
	wc.x = CB_ALGS::get_cost_pred<is_learn>(c.scorer, c.known_cost, ec, 1, 1);

	//add correction if we observed cost for this action and regressor is wrong
	if (c.known_cost != nullptr && c.known_cost->action == label)
		wc.x += (c.known_cost->cost - wc.x) / c.known_cost->probability;

	// do two-step pushes as for learn.
	COST_SENSITIVE::label cs_ld;
	cs_ld.costs.push_back(wc);
	array.push_back(cs_ld);
}

void gen_cs_example_dr(cb_adf& c, v_array<example*> examples, v_array<COST_SENSITIVE::label> array)
{
	for (example **ec = examples.begin; ec != examples.end; ec++)
	{
		// Get CB::label for each example/line.
		CB::label ld = (**ec).l.cb;

		if (ld.costs.size() == 1)  // 2nd line
		  gen_cs_label<true>(c, **ec, array, 1);
		else if (ld.costs.size() == 0)
		  gen_cs_label<false>(c, **ec, array, 1);
	}
}

  inline bool observed_cost(CB::cb_class* cl)
{
	//cost observed for this action if it has non zero probability and cost != FLT_MAX
	return (cl != nullptr && cl->cost != FLT_MAX && cl->probability > .0);
}

  CB::cb_class* get_observed_cost(CB::label ld)
{
  for (CB::cb_class *cl = ld.costs.begin; cl != ld.costs.end; cl++)
	{
		if (observed_cost(cl))
			return cl;
	}
	return nullptr;
}

template<bool is_learn>
void call_predict_or_learn(cb_adf& mydata, base_learner& base, v_array<example*> examples)
{
  // m2: still save, store, and restore
  // starting with 3 for loops
  // first of all, clear the container mydata.array.
  mydata.cb_labels.erase();
  
  // 1st: save cb_label (into mydata) and store cs_label for each example, which will be passed into base.learn.
  size_t index = 0;
  for (example **ec = examples.begin; ec != examples.end; ec++)
    {
      mydata.cb_labels.push_back((**ec).l.cb);
      (**ec).l.cs = mydata.cs_labels[index++]; 
    }
  
  // 2nd: predict for each ex
  // // call base.predict for each vw exmaple in the sequence
  for (example **ec = examples.begin; ec != examples.end; ec++)
    if (is_learn)
      base.learn(**ec);
    else
      base.predict(**ec);
  
  // 3rd: restore cb_label for each example
  // (**ec).l.cb = mydata.array.element.
  size_t i = 0;
  for (example **ec = examples.begin; ec != examples.end; ec++)
    (**ec).l.cb = mydata.cb_labels[i++];
}

template<uint32_t reduction_type>
void learn(cb_adf& mydata, base_learner& base, v_array<example*>& examples)
{
	// find the line/entry with cost and prob.
	CB::label ld;
	for (example **ec = examples.begin; ec != examples.end; ec++)
	  {
	    if ( (**ec).l.cb.costs.size() == 1 &&
		 (**ec).l.cb.costs[0].cost != FLT_MAX &&
		 (**ec).l.cb.costs[0].probability > 0)
	      {
		ld = (**ec).l.cb;
	      }
	  }
	
	mydata.known_cost = get_observed_cost(ld);
	
	if (mydata.known_cost == nullptr)
	  cerr << "known cost is null." << endl;
	
	if(reduction_type == CB_TYPE_IPS )
	  gen_cs_example_ips(examples, mydata.cs_labels);
	else 
	  {
	    std::cerr << "Unknown cb_type specified for contextual bandit learning: " << mydata.cb_type << ". Exiting." << endl;
	    throw exception();
	  }
	
	call_predict_or_learn<true>(mydata,base,examples);
}

bool test_adf_sequence(cb_adf& data)
{
  uint32_t count = 0;
  for (size_t k=0; k<data.ec_seq.size(); k++) {
    example *ec = data.ec_seq[k];
    
    if (ec->l.cb.costs.size() > 1)
      {
	cerr << "cb_adf: badly formatted example, only one cost can be known." << endl;	\
	throw exception();
      }
    
    if (ec->l.cb.costs.size() == 1 && ec->l.cb.costs[0].cost != FLT_MAX)      
      count += 1;
    
    if (CB::ec_is_example_header(*ec)) {
      cerr << "warning: example headers at position " << k << ": can only have in initial position!" << endl;
      throw exception();
    }
  }
  if (count == 0)
    return true;
  else if (count == 1)
    return false;
  else
    {
      cerr << "cb_adf: badly formatted example, only one line can have a cost" << endl; \
      throw exception();
    }
}

template <bool is_learn>
void do_actual_learning(cb_adf& data, base_learner& base)
{
  bool isTest = test_adf_sequence(data);
  
  if (isTest || !is_learn)
    {
      gen_cs_example_ips(data.ec_seq, data.cs_labels);//create test labels.
      call_predict_or_learn<false>(data, base, data.ec_seq);
    }
  else 
    learn<CB_TYPE_IPS>(data, base, data.ec_seq); 
}

void global_print_newline(vw& all)
{
  char temp[1];
  temp[0] = '\n';
  for (size_t i=0; i<all.final_prediction_sink.size(); i++) {
    int f = all.final_prediction_sink[i];
    ssize_t t;
    t = io_buf::write_file_or_socket(f, temp, 1);
    if (t != 1)
      cerr << "write error: " << strerror(errno) << endl;
  }
}

void output_example(vw& all, example& ec, bool& hit_loss, v_array<example*>* ec_seq)
{
  v_array<CB::cb_class> costs = ec.l.cb.costs;
    
  if (example_is_newline(ec)) return;
  if (CB::ec_is_example_header(ec)) return;

  all.sd->total_features += ec.num_features;

  float loss = 0.;

  if (!CB::example_is_test(ec)) {
    for (size_t j=0; j<costs.size(); j++) {
      if (hit_loss) break;
      if (ec.pred.multiclass == costs[j].action) {
        loss = costs[j].cost;
        hit_loss = true;
      }
    }

    all.sd->sum_loss += loss;
    all.sd->sum_loss_since_last_dump += loss;
    assert(loss >= 0);
  }
  
  for (int* sink = all.final_prediction_sink.begin; sink != all.final_prediction_sink.end; sink++)
    all.print(*sink, (float)ec.pred.multiclass, 0, ec.tag);

  if (all.raw_prediction > 0) {
    string outputString;
    stringstream outputStringStream(outputString);
    for (size_t i = 0; i < costs.size(); i++) {
      if (i > 0) outputStringStream << ' ';
      outputStringStream << costs[i].action << ':' << costs[i].partial_prediction;
    }
    //outputStringStream << endl;
    all.print_text(all.raw_prediction, outputStringStream.str(), ec.tag);
  }
    
  CB::print_update(all, CB::example_is_test(ec), ec, ec_seq);
}

void output_example_seq(vw& all, cb_adf& data)
{
  if (data.ec_seq.size() > 0) {
    all.sd->weighted_examples += 1;
    all.sd->example_number++;
    
    bool hit_loss = false;
    for (example** ecc=data.ec_seq.begin; ecc!=data.ec_seq.end; ecc++)
      output_example(all, **ecc, hit_loss, &(data.ec_seq));
    
    if (all.raw_prediction > 0)
      all.print_text(all.raw_prediction, "", data.ec_seq[0]->tag);
  }
}

void clear_seq_and_finish_examples(vw& all, cb_adf& data)
{
  if (data.ec_seq.size() > 0) 
    for (example** ecc=data.ec_seq.begin; ecc!=data.ec_seq.end; ecc++)
      if ((*ecc)->in_use)
        VW::finish_example(all, *ecc);
  data.ec_seq.erase();
}

void finish_multiline_example(vw& all, cb_adf& data, example& ec)
{
  if (data.need_to_clear) {
    if (data.ec_seq.size() > 0) {
      output_example_seq(all, data);
      global_print_newline(all);
    }        
    clear_seq_and_finish_examples(all, data);
    data.need_to_clear = false;
    if (ec.in_use) VW::finish_example(all, &ec);
  }
}

void end_examples(cb_adf& data)
{
  if (data.need_to_clear)
    data.ec_seq.erase();
}


void finish(cb_adf& data)
{
  data.ec_seq.delete_v();
  data.cb_labels.delete_v();
  for(size_t i = 0; i < data.cs_labels.size(); i++)
    data.cs_labels[i].costs.delete_v();
  data.cs_labels.delete_v();
}

template <bool is_learn>
void predict_or_learn(cb_adf& data, base_learner& base, example &ec) {
  vw* all = data.all;
  data.base = &base;
  bool is_test_ec = CB::example_is_test(ec);
  bool need_to_break = data.ec_seq.size() >= all->p->ring_size - 2;

  if ((example_is_newline(ec) && is_test_ec) || need_to_break) {
    data.ec_seq.push_back(&ec);
    do_actual_learning<is_learn>(data, base);
    data.need_to_clear = true;
  } else {
    if (data.need_to_clear) {  // should only happen if we're NOT driving
      data.ec_seq.erase();
      data.need_to_clear = false;
    }
    data.ec_seq.push_back(&ec);
  }
}
}

base_learner* cb_adf_setup(vw& all)
{
  if (missing_option(all, true, "cb_adf", "Do Contextual Bandit learning with multiline action dependent features."))
    return nullptr;
  
  cb_adf& ld = calloc_or_die<cb_adf>();
  
  ld.all = &all;

  if (count(all.args.begin(), all.args.end(),"--csoaa_ldf") == 0 && count(all.args.begin(), all.args.end(),"--wap_ldf") == 0)
    {
      all.args.push_back("--csoaa_ldf");
      all.args.push_back("multiline");
    }

  base_learner* base = setup_base(all);
  all.p->lp = CB::cb_label;

  learner<cb_adf>& l = init_learner(&ld, base, CB_ADF::predict_or_learn<true>, CB_ADF::predict_or_learn<false>);
  l.set_finish_example(CB_ADF::finish_multiline_example);
  l.set_finish(CB_ADF::finish);
  l.set_end_examples(CB_ADF::end_examples); 
  return make_base(l);
}
