// Copyright (c) 2017 GeometryFactory Sarl (France).
// All rights reserved.
//
// This file is part of CGAL (www.cgal.org).
// You can redistribute it and/or modify it under the terms of the GNU
// General Public License as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.
//
// Licensees holding a valid commercial license may use this file in
// accordance with the commercial license agreement provided with the software.
//
// This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
// WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
//
// $URL$
// $Id$
// SPDX-License-Identifier: GPL-3.0+
//
// Author(s)     : Simon Giraudot

#ifndef CGAL_CLASSIFICATION_ETHZ_RANDOM_FOREST_CLASSIFIER_H
#define CGAL_CLASSIFICATION_ETHZ_RANDOM_FOREST_CLASSIFIER_H

#include <CGAL/license/Classification.h>

#include <CGAL/Classification/Feature_set.h>
#include <CGAL/Classification/Label_set.h>
#include <CGAL/Classification/internal/verbosity.h>

#ifdef CGAL_CLASSIFICATION_VERBOSE
#define VERBOSE_TREE_PROGRESS 1
#endif

// Disable warnings from auxiliary library
#ifdef BOOST_MSVC
#  pragma warning(push)
#  pragma warning(disable:4141)
#  pragma warning(disable:4244)
#  pragma warning(disable:4267)
#  pragma warning(disable:4275)
#  pragma warning(disable:4251)
#  pragma warning(disable:4996)
#endif

#include <CGAL/Classification/ETHZ/internal/random-forest/node-gini.hpp>
#include <CGAL/Classification/ETHZ/internal/random-forest/forest.hpp>

#include <CGAL/tags.h>

#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>

#ifdef BOOST_MSVC
#  pragma warning(pop)
#endif


namespace CGAL {

namespace Classification {

namespace ETHZ {

/*!
  \ingroup PkgClassificationClassifiersETHZ

  \brief %Classifier based on the ETH Zurich version of the random forest algorithm \cgalCite{cgal:w-erftl-14}.

  \note This classifier is distributed under the MIT license.

  \cgalModels `CGAL::Classification::Classifier`
*/
class Random_forest_classifier
{
  typedef CGAL::internal::liblearning::RandomForest::RandomForest
  < CGAL::internal::liblearning::RandomForest::NodeGini
    < CGAL::internal::liblearning::RandomForest::AxisAlignedSplitter> > Forest;
  
  const Label_set& m_labels;
  const Feature_set& m_features;
  Forest* m_rfc;

public:
  
  /// \name Constructor
  /// @{
  
  /*!
    \brief Instantiates the classifier using the sets of `labels` and `features`.

  */
  Random_forest_classifier (const Label_set& labels,
                            const Feature_set& features)
    : m_labels (labels), m_features (features), m_rfc (nullptr)
  { }
  
  /*!
    \brief Copies the `other` classifier's configuration using another
    set of `features`.

    This constructor can be used to apply a trained random forest to
    another data set.

    \warning The feature set should be composed of the same features
    than the ones used by `other`, and in the same order.

  */
  Random_forest_classifier (const Random_forest_classifier& other,
                            const Feature_set& features)
    : m_labels (other.m_labels), m_features (features), m_rfc (nullptr)
  {
    std::stringstream stream;
    other.save_configuration(stream);
    this->load_configuration(stream);
  }
  
  /// \cond SKIP_IN_MANUAL
  ~Random_forest_classifier ()
  {
    if (m_rfc != nullptr)
      delete m_rfc;
  }
  /// \endcond
  
  /// @}

  /// \name Training
  /// @{

  /// \cond SKIP_IN_MANUAL
  template <typename LabelIndexRange>
  void train (const LabelIndexRange& ground_truth,
              bool reset_trees = true,
              std::size_t num_trees = 25,
              std::size_t max_depth = 20)
  {
#ifdef CGAL_LINKED_WITH_TBB
    train<CGAL::Parallel_tag>(ground_truth, reset_trees, num_trees, max_depth);
#else
    train<CGAL::Sequential_tag>(ground_truth, reset_trees, num_trees, max_depth);
#endif
  }
  /// \endcond
    
  /*!
    \brief Runs the training algorithm.

    From the set of provided ground truth, this algorithm estimates
    sets up the random trees that produce the most accurate result
    with respect to this ground truth.

    \pre At least one ground truth item should be assigned to each
    label.

    \tparam ConcurrencyTag enables sequential versus parallel
    algorithm. Possible values are `Parallel_tag` (default value is
    %CGAL is linked with TBB) or `Sequential_tag` (default value
    otherwise).

    \param ground_truth vector of label indices. It should contain for
    each input item, in the same order as the input set, the index of
    the corresponding label in the `Label_set` provided in the
    constructor. Input items that do not have a ground truth
    information should be given the value `-1`.

    \param reset_trees should be set to `false` if the users wants to
    _add_ new trees to the existing forest, and kept to `true` if the
    training should be recomputing from scratch (discarding the
    current forest).

    \param num_trees number of trees generated by the training
    algorithm. Higher values may improve result at the cost of higher
    computation times (in general, using a few dozens of trees is
    enough).

    \param max_depth maximum depth of the trees. Higher values will
    improve how the forest fits the training set. A overly low value
    will underfit the test data and conversely an overly high value
    will likely overfit.
  */
  template <typename ConcurrencyTag, typename LabelIndexRange>
  void train (const LabelIndexRange& ground_truth,
              bool reset_trees = true,
              std::size_t num_trees = 25,
              std::size_t max_depth = 20)
  {
    CGAL::internal::liblearning::RandomForest::ForestParams params;
    params.n_trees   = num_trees;
    params.max_depth = max_depth;

    std::vector<int> gt;
    std::vector<float> ft;
    
    for (std::size_t i = 0; i < ground_truth.size(); ++ i)
    {
      int g = int(ground_truth[i]);
      if (g != -1)
      {
        for (std::size_t f = 0; f < m_features.size(); ++ f)
          ft.push_back(m_features[f]->value(i));
        gt.push_back(g);
      }
    }

    CGAL_CLASSIFICATION_CERR << "Using " << gt.size() << " inliers" << std::endl;

    CGAL::internal::liblearning::DataView2D<int> label_vector (&(gt[0]), gt.size(), 1);    
    CGAL::internal::liblearning::DataView2D<float> feature_vector(&(ft[0]), gt.size(), ft.size() / gt.size());

    if (m_rfc != nullptr && reset_trees)
    {
      delete m_rfc;
      m_rfc = nullptr;
    }
    
    if (m_rfc == nullptr)
      m_rfc = new Forest (params);

    CGAL::internal::liblearning::RandomForest::AxisAlignedRandomSplitGenerator generator;
    
    m_rfc->train<ConcurrencyTag>
      (feature_vector, label_vector, CGAL::internal::liblearning::DataView2D<int>(), generator, 0, reset_trees, m_labels.size());
  }

  /// \cond SKIP_IN_MANUAL
  void operator() (std::size_t item_index, std::vector<float>& out) const
  {
    out.resize (m_labels.size(), 0.);
    
    std::vector<float> ft;
    ft.reserve (m_features.size());
    for (std::size_t f = 0; f < m_features.size(); ++ f)
      ft.push_back (m_features[f]->value(item_index));

    std::vector<float> prob (m_labels.size());

    m_rfc->evaluate (ft.data(), prob.data());
    
    for (std::size_t i = 0; i < out.size(); ++ i)
      out[i] = (std::min) (1.f, (std::max) (0.f, prob[i]));
  }

  /// \endcond
  
  /// @}

  /// \name Miscellaneous
  /// @{
  
  /*!
    \brief Computes, for each feature, how many nodes in the forest
    uses it as a split criterion.

    Each tree of the random forest recursively splits the training
    data set using at each node one of the input features. This method
    counts, for each feature, how many times it was selected by the
    training algorithm as a split criterion.

    This method allows to evaluate how useful a feature was with
    respect to a training set: if a feature is used a lot, that means
    that it has a strong discriminative power with respect to how the
    labels are represented by the feature set; on the contrary, if a
    feature is not used very often, its discriminative power is
    probably low; if a feature is _never_ used, it likely has no
    interest at all and is completely uncorrelated to the label
    segmentation of the training set.

    \param count vector where the result is stored. After running the
    method, it contains, for each feature, the number of nodes in the
    forest that use it as a split criterion, in the same order as the
    feature set order.
  */
  void get_feature_usage (std::vector<std::size_t>& count) const
  {
    count.clear();
    count.resize(m_features.size(), 0);
    return m_rfc->get_feature_usage(count);
  }
  
  /// @}

  /// \name Input/Output
  /// @{
  
  /*!
    \brief Saves the current configuration in the stream `output`.

    This allows to easily save and recover a specific classification
    configuration.

    The output file is written in an GZIP container that is readable
    by the `load_configuration()` method.
  */
  void save_configuration (std::ostream& output) const
  {
    boost::iostreams::filtering_ostream outs;
    outs.push(boost::iostreams::gzip_compressor());
    outs.push(output);
    boost::archive::text_oarchive oas(outs);
    oas << BOOST_SERIALIZATION_NVP(*m_rfc);
  }

  /*!
    \brief Loads a configuration from the stream `input`.

    The input file should be a GZIP container written by the
    `save_configuration()` method. The feature set of the classifier
    should contain the exact same features in the exact same order as
    the ones present when the file was generated using
    `save_configuration()`.
  */
  void load_configuration (std::istream& input)
  {
    CGAL::internal::liblearning::RandomForest::ForestParams params;
    if (m_rfc != nullptr)
      delete m_rfc;
    m_rfc = new Forest (params);
    
    boost::iostreams::filtering_istream ins;
    ins.push(boost::iostreams::gzip_decompressor());
    ins.push(input);
    boost::archive::text_iarchive ias(ins);
    ias >> BOOST_SERIALIZATION_NVP(*m_rfc);
  }

/// @}

};

}

/// \cond SKIP_IN_MANUAL
// Backward compatibility
typedef ETHZ::Random_forest_classifier ETHZ_random_forest_classifier;
/// \endcond

}

}

#endif // CGAL_CLASSIFICATION_ETHZ_RANDOM_FOREST_CLASSIFIER_H
