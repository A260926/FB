require 'tmpdir'
require 'fileutils'
require 'brakeman'
results = {}
[:unsafe_only, :safe_first, :unsafe_first].each do |mode|
 Dir.mktmpdir('brakeman-witness') do |root|
  safe = "def safe; render :partial => 'cards/card', :locals => { :value => 'hello' }; end\n"
  unsafe = "def unsafe; render :partial => 'cards/card', :locals => { :value => params[:q] }; end\n"
  actions = mode == :unsafe_only ? unsafe : (mode == :safe_first ? safe+unsafe : unsafe+safe)
  files = {
   'Gemfile' => "source 'https://rubygems.org'\ngem 'rails', '4.2.11'\n",
   'config/environment.rb' => '',
   'app/controllers/application_controller.rb' => "class ApplicationController < ActionController::Base; end\n",
   'app/controllers/cards_controller.rb' => "class CardsController < ApplicationController\n#{actions}end\n",
   'app/views/cards/_card.html.erb' => "<%= raw(value) %>\n<%= value %>\n"
  }
  files.each { |path, text| file=File.join(root,path); FileUtils.mkdir_p(File.dirname(file)); File.write(file,text) }
  tracker=Brakeman.run(:app_path=>root,:run_checks=>['CheckCrossSiteScripting'],:quiet=>true,:parallel_checks=>false)
  raise tracker.errors.inspect unless tracker.errors.empty?
  warnings=tracker.checks.all_warnings.select { |w| w.warning_type == 'Cross-Site Scripting' }
  raise 'unexpected template line' unless warnings.all? { |w| w.line == 1 }
  results[mode] = warnings.length
 end
end
puts results.inspect
raise 'starting behavior changed' unless results == {:unsafe_only=>1, :safe_first=>0, :unsafe_first=>1}
puts 'BASELINE_RENDER_CACHE unsafe_only=1 safe_first=0 unsafe_first=1'
