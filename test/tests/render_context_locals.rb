require_relative '../test'
require 'tmpdir'
require 'fileutils'
require 'brakeman'

#Tests that the context (especially :locals and :collection) of a render call
#is applied to the template before the render cache is consulted.
#
#Two actions may render the same template with different values. The template
#processing for each action must be independent: a safe render must not hide a
#dangerous one, and a warning must be attributed to the action that actually
#supplied the dangerous value.
class RenderContextLocalsTests < Minitest::Test
  BASE_FILES = {
    'Gemfile' => "source 'https://rubygems.org'\ngem 'rails', '4.2.11'\n",
    'config/environment.rb' => '',
    'app/controllers/application_controller.rb' => "class ApplicationController < ActionController::Base; end\n"
  }

  UNSAFE_ACTION = "def unsafe; render :partial => 'cards/card', :locals => { :value => params[:q] }; end\n"
  SAFE_ACTION = "def safe; render :partial => 'cards/card', :locals => { :value => 'hello' }; end\n"

  #Line 1 is static, so a warning on line 2 proves line numbers are preserved.
  CARD_VIEW = <<-ERB
<h2>Card</h2>
<%= raw(value) %>
<%= value %>
<%= h(value) %>
<p>constant output</p>
  ERB

  def scan_app(files)
    tracker = nil

    Dir.mktmpdir("brakeman-render-context") do |root|
      BASE_FILES.merge(files).each do |path, contents|
        full_path = File.join(root, path)
        FileUtils.mkdir_p(File.dirname(full_path))
        File.write(full_path, contents)
      end

      tracker = Brakeman.run(:app_path => root,
                             :run_checks => ['CheckCrossSiteScripting'],
                             :quiet => true,
                             :parallel_checks => false)
    end

    assert_empty tracker.errors
    tracker
  end

  def xss_warnings(tracker)
    tracker.checks.all_warnings.select { |w| w.warning_type == "Cross-Site Scripting" }
  end

  def assert_xss_warning(warning, file, line, code, message, called_from)
    assert_equal file, warning.relative_path
    assert_equal line, warning.line
    assert_equal code, warning.format_code(false)
    assert_equal message, warning.message
    assert_equal 0, warning.confidence
    assert_equal called_from, warning.called_from.to_a
  end

  def controller_source(actions)
    "class CardsController < ApplicationController\n#{actions}end\n"
  end

  #The order of actions in the controller source must not affect results.
  def test_locals_order
    variations = {
      :unsafe_only => UNSAFE_ACTION,
      :safe_first => SAFE_ACTION + UNSAFE_ACTION,
      :unsafe_first => UNSAFE_ACTION + SAFE_ACTION
    }

    variations.each do |name, actions|
      tracker = scan_app(
        'app/controllers/cards_controller.rb' => controller_source(actions),
        'app/views/cards/_card.html.erb' => CARD_VIEW
      )

      warnings = xss_warnings(tracker)

      assert_equal 1, warnings.length, "Expected one XSS warning for #{name}, got: #{warnings.map(&:to_s).inspect}"

      warning = warnings.first
      assert_xss_warning warning,
        "app/views/cards/_card.html.erb",
        2,
        "params[:q]",
        "Unescaped parameter value",
        ["CardsController#unsafe"]

      #The constant, the escaped output, and h() must not add warnings.
      refute warnings.any? { |w| w.format_code(false).include?("hello") }
      refute warnings.any? { |w| w.format_code(false).include?("h(") }
      refute warnings.any? { |w| w.called_from.to_a.include?("CardsController#safe") }
    end
  end

  #The controller passes a value to a wrapper, which passes it on to a shared
  #partial under a different local name. The shared partial must be analyzed
  #with the environment of the action at the root of the render path.
  def test_nested_context
    item_view = <<-ERB
<h2>Item</h2>
<%= raw(payload) %>
<%= payload %>
<%= h(payload) %>
    ERB

    wrapper_view = "<%= render 'cards/middle', :locals => { :thing => value } %>\n"
    middle_view = "<%= render 'shared/item', :locals => { :payload => thing } %>\n"

    safe = "def safe; render :partial => 'cards/wrapper', :locals => { :value => 'hello' }; end\n"
    unsafe = "def unsafe; render :partial => 'cards/wrapper', :locals => { :value => params[:q] }; end\n"

    { :safe_first => safe + unsafe, :unsafe_first => unsafe + safe }.each do |name, actions|
      tracker = scan_app(
        'app/controllers/cards_controller.rb' => controller_source(actions),
        'app/views/cards/_wrapper.html.erb' => wrapper_view,
        'app/views/cards/_middle.html.erb' => middle_view,
        'app/views/shared/_item.html.erb' => item_view
      )

      warnings = xss_warnings(tracker)

      assert_equal 1, warnings.length, "Expected one XSS warning for #{name}, got: #{warnings.map(&:to_s).inspect}"

      warning = warnings.first
      assert_xss_warning warning,
        "app/views/shared/_item.html.erb",
        2,
        "params[:q]",
        "Unescaped parameter value",
        ["CardsController#unsafe", "Template:cards/_wrapper", "Template:cards/_middle"]

      refute warnings.any? { |w| w.called_from.to_a.first == "CardsController#safe" }
    end
  end

  #A collection rendered with :as is a model value and keeps its existing
  #taint result. It must not be lost when the same partial is also rendered
  #with constant locals, in either order.
  def test_collection_as_not_mixed_with_constant_locals
    item_view = <<-ERB
<h2>Item</h2>
<%= raw(card) %>
<%= card %>
<%= h(card) %>
    ERB

    collection_action = "def unsafe; @cards = Card.all; render :partial => 'cards/item', :collection => @cards, :as => :card; end\n"
    locals_action = "def safe; render :partial => 'cards/item', :locals => { :card => 'hello' }; end\n"

    { :unsafe_first => collection_action + locals_action,
      :safe_first => locals_action + collection_action }.each do |name, actions|
      tracker = scan_app(
        'app/models/card.rb' => "class Card < ActiveRecord::Base; end\n",
        'app/controllers/cards_controller.rb' => controller_source(actions),
        'app/views/cards/_item.html.erb' => item_view
      )

      warnings = xss_warnings(tracker)

      assert_equal 1, warnings.length, "Expected one XSS warning for #{name}, got: #{warnings.map(&:to_s).inspect}"

      warning = warnings.first
      assert_xss_warning warning,
        "app/views/cards/_item.html.erb",
        2,
        "Card.new",
        "Unescaped model attribute",
        ["CardsController#unsafe"]
    end
  end

  #Iterating over a collection of models and passing an attribute through
  #locals must still be treated as model output.
  def test_loop_context
    list_view = <<-ERB
<% @cards.each do |card| %>
<%= render :partial => 'cards/item', :locals => { :value => card.name } %>
<% end %>
    ERB

    item_view = "<h2>Item</h2>\n<%= raw(value) %>\n<%= value %>\n"

    tracker = scan_app(
      'app/models/card.rb' => "class Card < ActiveRecord::Base; end\n",
      'app/controllers/cards_controller.rb' => "class CardsController < ApplicationController\ndef unsafe; @cards = Card.all; render :partial => 'cards/list'; end\nend\n",
      'app/views/cards/_list.html.erb' => list_view,
      'app/views/cards/_item.html.erb' => item_view
    )

    warnings = xss_warnings(tracker)

    assert_equal 1, warnings.length, "Expected one XSS warning, got: #{warnings.map(&:to_s).inspect}"

    assert_xss_warning warnings.first,
      "app/views/cards/_item.html.erb",
      2,
      "Card.new.name",
      "Unescaped model attribute",
      ["CardsController#unsafe", "Template:cards/_list"]
  end

  #Two branches of the same template render different partials with dangerous
  #values under different local names. Both paths must be reported.
  def test_branches_keep_both_dangerous_paths
    branch_view = <<-ERB
<% if params[:x] %>
<%= render 'cards/left', :locals => { :value => params[:q] } %>
<% else %>
<%= render 'cards/right', :locals => { :other => params[:r] } %>
<% end %>
    ERB

    tracker = scan_app(
      'app/controllers/cards_controller.rb' => "class CardsController < ApplicationController\ndef index; render :partial => 'cards/branch'; end\nend\n",
      'app/views/cards/_branch.html.erb' => branch_view,
      'app/views/cards/_left.html.erb' => "<h2>Leaf</h2>\n<%= raw(value) %>\n",
      'app/views/cards/_right.html.erb' => "<h2>Leaf</h2>\n<%= raw(other) %>\n"
    )

    warnings = xss_warnings(tracker)
    assert_equal 2, warnings.length, "Expected both dangerous branches, got: #{warnings.map(&:to_s).inspect}"

    left = warnings.find { |w| w.relative_path == "app/views/cards/_left.html.erb" }
    right = warnings.find { |w| w.relative_path == "app/views/cards/_right.html.erb" }

    refute_nil left, "Missing warning for the left branch"
    refute_nil right, "Missing warning for the right branch"

    assert_xss_warning left, "app/views/cards/_left.html.erb", 2, "params[:q]",
      "Unescaped parameter value", ["CardsController#index", "Template:cards/_branch"]
    assert_xss_warning right, "app/views/cards/_right.html.erb", 2, "params[:r]",
      "Unescaped parameter value", ["CardsController#index", "Template:cards/_branch"]
  end

  #A shared sub-template reached through two different wrappers must be
  #analyzed once per reachable context. Neither wrapper may clear the
  #state of the other, whichever branch is visited first.
  def test_shared_partial_from_two_wrappers
    #Line 2 is only dangerous through the first wrapper, line 3 only through
    #the second, so the existing per-template/per-line deduplication cannot
    #hide either one.
    item_view = "<h2>Item</h2>\n<%= raw(thing) %>\n<%= raw(other) %>\n"

    branch_view = {
      :wrap_a_first => "<% if params[:x] %>\n<%= render 'cards/wrap_a', :locals => { :value => params[:q] } %>\n<% else %>\n<%= render 'cards/wrap_b', :locals => { :other => params[:r] } %>\n<% end %>\n",
      :wrap_b_first => "<% if params[:x] %>\n<%= render 'cards/wrap_b', :locals => { :other => params[:r] } %>\n<% else %>\n<%= render 'cards/wrap_a', :locals => { :value => params[:q] } %>\n<% end %>\n"
    }

    branch_view.each do |name, view|
      tracker = scan_app(
        'app/controllers/cards_controller.rb' => "class CardsController < ApplicationController\ndef index; render :partial => 'cards/branch'; end\nend\n",
        'app/views/cards/_branch.html.erb' => view,
        'app/views/cards/_wrap_a.html.erb' => "<%= render 'shared/item', :locals => { :thing => value } %>\n",
        'app/views/cards/_wrap_b.html.erb' => "<%= render 'shared/item', :locals => { :other => other } %>\n",
        'app/views/shared/_item.html.erb' => item_view
      )

      warnings = xss_warnings(tracker)

      assert_equal 2, warnings.length, "Expected both wrapper contexts for #{name}, got: #{warnings.map(&:to_s).inspect}"

      by_line = warnings.sort_by(&:line)
      assert_xss_warning by_line[0],
        "app/views/shared/_item.html.erb", 2, "params[:q]", "Unescaped parameter value",
        ["CardsController#index", "Template:cards/_branch", "Template:cards/_wrap_a"]
      assert_xss_warning by_line[1],
        "app/views/shared/_item.html.erb", 3, "params[:r]", "Unescaped parameter value",
        ["CardsController#index", "Template:cards/_branch", "Template:cards/_wrap_b"]
    end
  end

  #A dangerous branch followed or preceded by a safe render of the same
  #partial must report only the dangerous one, regardless of branch order.
  def test_branch_safe_and_dangerous_same_partial
    views = {
      :danger_in_then => "<% if params[:x] %>\n<%= render 'cards/card', :locals => { :value => params[:q] } %>\n<% else %>\n<%= render 'cards/card', :locals => { :value => 'hello' } %>\n<% end %>\n",
      :danger_in_else => "<% if params[:x] %>\n<%= render 'cards/card', :locals => { :value => 'hello' } %>\n<% else %>\n<%= render 'cards/card', :locals => { :value => params[:q] } %>\n<% end %>\n"
    }

    views.each do |name, view|
      tracker = scan_app(
        'app/controllers/cards_controller.rb' => "class CardsController < ApplicationController\ndef index; render :partial => 'cards/branch'; end\nend\n",
        'app/views/cards/_branch.html.erb' => view,
        'app/views/cards/_card.html.erb' => "<h2>Card</h2>\n<%= raw(value) %>\n<%= value %>\n"
      )

      warnings = xss_warnings(tracker)
      assert_equal 1, warnings.length, "Expected one XSS warning for #{name}, got: #{warnings.map(&:to_s).inspect}"

      assert_xss_warning warnings.first,
        "app/views/cards/_card.html.erb", 2, "params[:q]", "Unescaped parameter value",
        ["CardsController#index", "Template:cards/_branch"]
    end
  end

  #Recursive and mutually recursive partials must terminate, and the shared
  #partial at the end of the cycle must still be reported with the full path.
  def test_recursive_partials_terminate
    tracker = scan_app(
      'app/controllers/cards_controller.rb' => controller_source(UNSAFE_ACTION.sub("cards/card", "cards/a")),
      'app/views/cards/_a.html.erb' => "<%= render 'cards/b', :locals => { :value => value } %>\n",
      'app/views/cards/_b.html.erb' => "<%= render 'cards/a', :locals => { :value => value } %>\n<%= render 'cards/leaf', :locals => { :value => value } %>\n",
      'app/views/cards/_leaf.html.erb' => "<h2>Leaf</h2>\n<%= raw(value) %>\n"
    )

    warnings = xss_warnings(tracker)
    assert_equal 1, warnings.length, "Expected one XSS warning, got: #{warnings.map(&:to_s).inspect}"

    assert_xss_warning warnings.first,
      "app/views/cards/_leaf.html.erb",
      2,
      "params[:q]",
      "Unescaped parameter value",
      ["CardsController#unsafe", "Template:cards/_a", "Template:cards/_b"]
  end

  #Identical contexts may reuse the cached result.
  def test_same_context_reuses_cache
    tracker = scan_app(
      'app/controllers/cards_controller.rb' => controller_source(
        UNSAFE_ACTION.sub("def unsafe", "def one") + UNSAFE_ACTION.sub("def unsafe", "def two")),
      'app/views/cards/_card.html.erb' => CARD_VIEW
    )

    warnings = xss_warnings(tracker)
    assert_equal 1, warnings.length, "Identical contexts should share cached processing"
    assert_equal ["CardsController#one"], warnings.first.called_from.to_a
  end

  #Alias processing must not rewrite the pristine template source. Only the
  #per-render copies may contain the resolved values.
  def test_template_source_is_not_mutated
    tracker = scan_app(
      'app/controllers/cards_controller.rb' => controller_source(SAFE_ACTION + UNSAFE_ACTION),
      'app/views/cards/_card.html.erb' => CARD_VIEW
    )

    original = tracker.templates[:"cards/_card"]
    refute_nil original

    refute original.src.to_s.include?('"hello"'), "Original template source was aliased with a safe value"
    refute original.src.to_s.include?("params"), "Original template source was aliased with user input"

    safe_name = tracker.templates.keys.find { |k| k.to_s.include?("CardsController#safe") }
    unsafe_name = tracker.templates.keys.find { |k| k.to_s.include?("CardsController#unsafe") }

    refute_nil safe_name, "Safe render was not processed"
    refute_nil unsafe_name, "Unsafe render was not processed"

    assert tracker.templates[safe_name].src.to_s.include?('"hello"')
    assert tracker.templates[unsafe_name].src.to_s.include?("params")
  end

  #Each Brakeman.run gets its own tracker; scanning a second application in
  #the same process must not inherit the first application's render cache.
  def test_scanning_two_applications_does_not_leak
    unsafe_app = {
      'app/controllers/cards_controller.rb' => controller_source(UNSAFE_ACTION),
      'app/views/cards/_card.html.erb' => CARD_VIEW
    }
    safe_app = {
      'app/controllers/cards_controller.rb' => controller_source(SAFE_ACTION),
      'app/views/cards/_card.html.erb' => CARD_VIEW
    }

    first = xss_warnings(scan_app(unsafe_app))
    assert_equal 1, first.length
    assert_equal ["CardsController#unsafe"], first.first.called_from.to_a

    second = xss_warnings(scan_app(safe_app))
    assert_equal 0, second.length, "Safe application inherited a warning from the previous scan"

    third = xss_warnings(scan_app(unsafe_app))
    assert_equal 1, third.length, "Unsafe application was skipped because of a previous scan"
    assert_equal ["CardsController#unsafe"], third.first.called_from.to_a
  end
end
