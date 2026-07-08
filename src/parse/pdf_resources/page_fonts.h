//-*-C++-*-

#ifndef PDF_PAGE_FONTS_RESOURCE_H
#define PDF_PAGE_FONTS_RESOURCE_H

#include <cstdlib>
#include <map>
#include <memory>
#include <tuple>

namespace pdflib
{

  template<>
  class pdf_resource<PAGE_FONTS>
  {
  public:

    pdf_resource();
    pdf_resource(std::shared_ptr<pdf_resource<PAGE_FONTS>> parent);
    ~pdf_resource();

    nlohmann::json get();

    size_t size();

    int count(std::string key);

    std::unordered_set<std::string> keys();

    pdf_resource<PAGE_FONT>& operator[](std::string fort_name);

    void set(QPDFObjectHandle& qpdf_fonts_,
             pdf_timings& timings);

  private:

    std::shared_ptr<pdf_resource<PAGE_FONTS>> parent_;

    // Parsed fonts are held by shared_ptr and memoized (see set()); they are
    // read-only after parsing, so a single instance can be shared safely.
    std::unordered_map<std::string, std::shared_ptr<pdf_resource<PAGE_FONT> > > page_fonts;
  };

  pdf_resource<PAGE_FONTS>::pdf_resource():
    parent_(nullptr)
  {}

  pdf_resource<PAGE_FONTS>::pdf_resource(std::shared_ptr<pdf_resource<PAGE_FONTS>> parent):
    parent_(parent)
  {}

  pdf_resource<PAGE_FONTS>::~pdf_resource()
  {}

  nlohmann::json pdf_resource<PAGE_FONTS>::get()
  {
    nlohmann::json result;
    {
      for(auto itr=page_fonts.begin(); itr!=page_fonts.end(); itr++)
        {
          result[itr->first] = (itr->second)->get();
        }
    }
    
    return result;
  }

  size_t pdf_resource<PAGE_FONTS>::size()
  {
    return page_fonts.size();
  }

  int pdf_resource<PAGE_FONTS>::count(std::string key)
  {
    if(page_fonts.count(key)==1)
      {
        return 1;
      }
    if(parent_)
      {
        return parent_->count(key);
      }
    return 0;
  }

  std::unordered_set<std::string> pdf_resource<PAGE_FONTS>::keys()
  {
    std::unordered_set<std::string> keys_;

    if(parent_)
      {
        keys_ = parent_->keys();
      }

    for(auto itr=page_fonts.begin(); itr!=page_fonts.end(); itr++)
      {
        keys_.insert(itr->first);
      }

    return keys_;
  }

  pdf_resource<PAGE_FONT>& pdf_resource<PAGE_FONTS>::operator[](std::string font_name)
  {
    if(page_fonts.count(font_name)==1)
      {
        return *(page_fonts.at(font_name));
      }

    if(parent_)
      {
        return (*parent_)[font_name];
      }

    {
      std::stringstream ss;
      ss << "font_name [" << font_name << "] is not known: ";
      for(auto itr=page_fonts.begin(); itr!=page_fonts.end(); itr++)
        {
          if(itr==page_fonts.begin())
            {
              ss << itr->first;
            }
          else
            {
              ss << ", " << itr->first;
            }
        }

      throw std::logic_error(ss.str());
    }

    return *(page_fonts.begin()->second);
  }

  // Memoization cache for parsed fonts.
  //
  // do_form() re-parses the invoked form-XObject's fonts on every invocation.
  // Pathological pages stamp the same handful of form XObjects hundreds of
  // thousands of times, all referencing the same (indirect) font objects, so
  // the same fonts would otherwise be re-serialized (to_json) and re-parsed
  // (encoding/cmap/widths and several std::regex compiles) millions of times
  // per page. A parsed font is a pure, read-only function of (font object,
  // local font key), so it is memoized here by (owning QPDF, object id+gen,
  // key) and shared via shared_ptr.
  //
  // The 'key' is part of the cache key because two local names may point at the
  // same font object; the parsed font records its font_key, and downstream
  // (enforce_same_font) merging distinguishes cells by it.
  //
  // The cache is thread_local and scoped to a single document: it is cleared
  // whenever the owning QPDF changes. This keeps it correct across sequential
  // documents (object ids are only unique within one document) and race-free on
  // the threaded parser path. Only indirect font objects are cached. Disable
  // with the environment variable DP_DISABLE_FONT_CACHE=1.
  static bool dp_font_cache_enabled()
  {
    static const bool enabled = (std::getenv("DP_DISABLE_FONT_CACHE") == nullptr);
    return enabled;
  }

  void pdf_resource<PAGE_FONTS>::set(QPDFObjectHandle& qpdf_fonts,
                                     pdf_timings& timings)
  {
    LOG_S(INFO) << __FUNCTION__;

    static thread_local QPDF* font_cache_owner = nullptr;
    static thread_local std::map<std::tuple<QPDFObjGen, std::string>,
                                 std::shared_ptr<pdf_resource<PAGE_FONT> > > font_cache;

    const bool cache_on = dp_font_cache_enabled();

    double total_font_time = 0.0;

    for(auto& key : qpdf_fonts.getKeys())
      {
        LOG_S(INFO) << "decoding font: " << key;

	utils::timer font_timer;

	QPDFObjectHandle qpdf_font = qpdf_fonts.getKey(key);

	const bool cacheable = cache_on && qpdf_font.isIndirect();

	std::shared_ptr<pdf_resource<PAGE_FONT> > page_font;

	if(cacheable)
	  {
	    QPDF* owner = qpdf_font.getOwningQPDF();
	    if(owner != font_cache_owner)
	      {
		font_cache.clear();
		font_cache_owner = owner;
	      }

	    auto ck = std::make_tuple(qpdf_font.getObjGen(), key);
	    auto itr = font_cache.find(ck);
	    if(itr != font_cache.end())
	      {
		page_font = itr->second;
	      }
	    else
	      {
		nlohmann::json json_font = to_json(qpdf_font);
		page_font = std::make_shared<pdf_resource<PAGE_FONT> >(timings);
		page_font->set(key, json_font, qpdf_font);
		font_cache[ck] = page_font;
	      }
	  }
	else
	  {
	    nlohmann::json json_font = to_json(qpdf_font);
	    page_font = std::make_shared<pdf_resource<PAGE_FONT> >(timings);
	    page_font->set(key, json_font, qpdf_font);
	  }

	if(page_fonts.count(key)==1)
	  {
	    LOG_S(WARNING) << "We are overwriting a font!";
	    page_fonts.erase(key);
	  }

	page_fonts.emplace(key, page_font);

	double font_time = font_timer.get_time();
	total_font_time += font_time;
	// per-font (dynamic) timing disabled for now; only the total is reported
	//timings.add_timing(pdf_timings::PREFIX_DECODE_FONT + key, font_time);
      }

    timings.add_timing(pdf_timings::KEY_DECODE_FONTS_TOTAL, total_font_time);
    timings.note_attributed(total_font_time);
  }

}

#endif
