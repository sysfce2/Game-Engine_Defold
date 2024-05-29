;; Copyright 2020-2024 The Defold Foundation
;; Copyright 2014-2020 King
;; Copyright 2009-2014 Ragnar Svensson, Christian Murray
;; Licensed under the Defold License version 1.0 (the "License"); you may not use
;; this file except in compliance with the License.
;;
;; You may obtain a copy of the License, together with FAQs at
;; https://www.defold.com/license
;;
;; Unless required by applicable law or agreed to in writing, software distributed
;; under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
;; CONDITIONS OF ANY KIND, either express or implied. See the License for the
;; specific language governing permissions and limitations under the License.

(ns editor.editor-extensions
  (:require [cljfx.api :as fx]
            [clojure.java.io :as io]
            [clojure.spec.alpha :as s]
            [clojure.stacktrace :as stacktrace]
            [clojure.string :as string]
            [dynamo.graph :as g]
            [editor.code.data :as data]
            [editor.code.util :as code.util]
            [editor.console :as console]
            [editor.defold-project :as project]
            [editor.error-reporting :as error-reporting]
            [editor.editor-extensions.runtime :as rt]
            [editor.editor-extensions.validation :as validation]
            [editor.editor-extensions.vm :as vm]
            [editor.fs :as fs]
            [editor.future :as future]
            [editor.graph-util :as gu]
            [editor.handler :as handler]
            [editor.lsp :as lsp]
            [editor.lsp.async :as lsp.async]
            [editor.outline :as outline]
            [editor.process :as process]
            [editor.properties :as properties]
            [editor.resource :as resource]
            [editor.system :as system]
            [editor.types :as types]
            [editor.util :as util]
            [editor.workspace :as workspace])
  (:import [clojure.lang MultiFn]
           [com.dynamo.bob Platform]
           [java.nio.file FileAlreadyExistsException Files LinkOption NotDirectoryException Path]
           [org.luaj.vm2 LuaError Prototype]))

(defn- ext-state
  "Returns an extension state, a map with following keys:
    :reload-resources      0-arg function used to reload resources
    :can-execute?          1-arg function of command vector used to ask the user
                           if it's okay to execute this command
    :display-output        2-arg function used to display extension-related
                           output to the user, where args are:
                             type    output type, :err or :out
                             msg     string message, may be multiline
    :project-prototypes    vector of project-owned editor script Prototypes
    :library-prototypes    vector of library-provided editor script Prototypes
    :rt                    editor script runtime
    :all                   map of module function keyword to a vector of tuples:
                             path      proj-path of an editor script
                             lua-fn    LuaFunction identified by the keyword
    :hooks                 exists only when '/hooks.editor_script' exists, a map
                           from module function keyword to LuaFunction"
  [project evaluation-context]
  (-> project
      (g/node-value :editor-extensions evaluation-context)
      (g/user-data :state)))

(defn- display-script-error [display-output label ex]
  (display-output :err (str label " failed: " (or (ex-message ex) (.getSimpleName (class ex))))))

(s/fdef try-with-extension-exceptions
  :args (s/cat :kv-args (s/+ (s/cat :k #{:display-output :label :catch} :v any?)) :expr any?))
(defmacro try-with-extension-exceptions
  "Convenience macro for executing an expression and reporting extension errors
  to the console

  Leading kv-args:
    :display-output    require, 2-arg fn to display error output
    :label             optional, string error prefix
    :catch             optional, result value in case of exceptions (otherwise
                       the Exception is re-thrown)"
  [& kv-args+expr]
  (let [opts (apply hash-map (butlast kv-args+expr))
        expr (last kv-args+expr)
        ex-sym (gensym "ex")]
    (when-not (:display-output opts)
      (throw (Exception. ":display-output is required")))
    `(try
       ~expr
       (catch Exception ~ex-sym
         (display-script-error ~(:display-output opts) ~(:label opts "Extension") ~ex-sym)
         ~(:catch opts `(throw ~ex-sym))))))

(defn- execute-all-top-level-functions
  "Returns reducible that executes all specified top-level editor script fns

  Args:
    state         editor extensions state
    fn-keyword    keyword identifying the editor script function
    opts          Clojure data structure that will be coerced to Lua

  Returns a vector of path+ret tuples, removing all results that threw
  exception, where:
    path    a proj-path of the editor script, string
    ret     a Clojure data structure returned from function in that file"
  [state fn-keyword opts evaluation-context]
  (let [{:keys [rt all display-output]} state
        lua-opts (rt/->lua opts)
        label (name fn-keyword)]
    (eduction
      (keep (fn [[path lua-fn]]
              (when-let [ret (try-with-extension-exceptions
                               :display-output display-output
                               :label (str label " in " path)
                               :catch nil
                               (rt/->clj rt (rt/invoke-immediate rt lua-fn lua-opts evaluation-context)))]
                [path ret])))
      (get all fn-keyword))))

(defn- unwrap-error-values [arr]
  (mapv #(cond-> % (g/error? %) :value) arr))

(g/defnode EditorExtensions
  (input project-prototypes g/Any :array :substitute unwrap-error-values)
  (input library-prototypes g/Any :array :substitute unwrap-error-values)
  (output project-prototypes g/Any (gu/passthrough project-prototypes))
  (output library-prototypes g/Any (gu/passthrough library-prototypes)))

(defn make [graph]
  (first (g/tx-nodes-added (g/transact (g/make-node graph EditorExtensions)))))

;; region API

;; region get
(defn- node-id->type-keyword [node-id ec]
  (g/node-type-kw (:basis ec) node-id))

(defn- node-id-or-path->node-id [node-id-or-path project evaluation-context]
  (if (string? node-id-or-path)
    (let [node-id (project/get-resource-node project node-id-or-path evaluation-context)]
      (when (nil? node-id)
        (throw (LuaError. (str node-id-or-path " not found"))))
      node-id)
    node-id-or-path))

(defn- ensuring-converter [spec]
  (fn [value _outline-property _project _evaluation-context]
    (validation/ensure spec value)))

(defn- resource-converter [node-id-or-path outline-property project evaluation-context]
  (validation/ensure (s/or :nothing #{""} :resource ::validation/node-id) node-id-or-path)
  (when-not (= node-id-or-path "")
    (let [node-id (node-id-or-path->node-id node-id-or-path project evaluation-context)
          resource (g/node-value node-id :resource evaluation-context)
          ext (:ext (properties/property-edit-type outline-property))]
      (when (seq ext)
        (validation/ensure (set ext) (resource/type-ext resource)))
      resource)))

(def ^:private edit-type-id->value-converter
  {g/Str {:to identity :from (ensuring-converter string?)}
   g/Bool {:to identity :from (ensuring-converter boolean?)}
   g/Num {:to identity :from (ensuring-converter number?)}
   g/Int {:to identity :from (ensuring-converter int?)}
   types/Vec2 {:to identity :from (ensuring-converter (s/tuple number? number?))}
   types/Vec3 {:to identity :from (ensuring-converter (s/tuple number? number? number?))}
   types/Vec4 {:to identity :from (ensuring-converter (s/tuple number? number? number? number?))}
   'editor.resource.Resource {:to resource/proj-path :from resource-converter}})

(defn- multi-responds? [^MultiFn multi & args]
  (some? (.getMethod multi (apply (.-dispatchFn multi) args))))

(defn- property->prop-kw [property]
  (if (string/starts-with? property "__")
    (keyword property)
    (keyword (string/replace property "_" "-"))))

(defn- outline-property [node-id property ec]
  (when (g/node-instance? (:basis ec) outline/OutlineNode node-id)
    (let [prop-kw (property->prop-kw property)
          outline-property (-> node-id
                               (g/node-value :_properties ec)
                               (get-in [:properties prop-kw]))]
      (when (and outline-property
                 (properties/visible? outline-property)
                 (edit-type-id->value-converter (properties/edit-type-id outline-property)))
        (cond-> outline-property
                (not (contains? outline-property :prop-kw))
                (assoc :prop-kw prop-kw))))))

(defmulti ext-get (fn [node-id property ec]
                    [(node-id->type-keyword node-id ec) property]))

(defmethod ext-get [:editor.code.resource/CodeEditorResourceNode "text"] [node-id _ ec]
  (clojure.string/join \newline (g/node-value node-id :lines ec)))

(defmethod ext-get [:editor.resource/ResourceNode "path"] [node-id _ ec]
  (resource/resource->proj-path (g/node-value node-id :resource ec)))

(defn- ext-value-getter [node-id property evaluation-context]
  (if (multi-responds? ext-get node-id property evaluation-context)
    #(ext-get node-id property evaluation-context)
    (if-let [outline-property (outline-property node-id property evaluation-context)]
      (when-let [to (-> outline-property
                        properties/edit-type-id
                        edit-type-id->value-converter
                        :to)]
        #(some-> (properties/value outline-property) to))
      nil)))

;; endregion

;; region set

(defmulti ext-setter
  "Returns a function that receives value and returns txs"
  (fn [node-id property evaluation-context]
    [(node-id->type-keyword node-id evaluation-context) property]))

(defmethod ext-setter [:editor.code.resource/CodeEditorResourceNode "text"]
  [node-id _ _]
  (fn [value]
    [(g/set-property node-id :modified-lines (code.util/split-lines value))
     (g/update-property node-id :invalidated-rows conj 0)
     (g/set-property node-id :cursor-ranges [#code/range[[0 0] [0 0]]])
     (g/set-property node-id :regions [])]))

(defn- ext-value-setter [node-id property project evaluation-context]
  (if (multi-responds? ext-setter node-id property evaluation-context)
    (ext-setter node-id property evaluation-context)
    (if-let [outline-property (outline-property node-id property evaluation-context)]
      (when-not (properties/read-only? outline-property)
        (when-let [from (-> outline-property
                            properties/edit-type-id
                            edit-type-id->value-converter
                            :from)]
          #(properties/set-value evaluation-context
                                 outline-property
                                 (from % outline-property project evaluation-context))))
      nil)))

;; endregion

;; region definitions

(defn- mk-ext-get [project]
  (rt/lua-fn [lua-node-id-or-path lua-property]
    (let [{:keys [rt evaluation-context]} (rt/current-execution-context)
          node-id-or-path (validation/ensure ::validation/node-id (rt/->clj rt lua-node-id-or-path))
          property (validation/ensure string? (rt/->clj rt lua-property))
          node-id (node-id-or-path->node-id node-id-or-path project evaluation-context)
          getter (ext-value-getter node-id property evaluation-context)]
      (if getter
        (getter)
        (throw (LuaError. (str (name (node-id->type-keyword node-id evaluation-context))
                               " has no \""
                               property
                               "\" property")))))))

(defn- mk-ext-can-get [project]
  (rt/lua-fn [lua-node-id-or-path lua-property]
    (let [{:keys [rt evaluation-context]} (rt/current-execution-context)
          node-id-or-path (validation/ensure ::validation/node-id (rt/->clj rt lua-node-id-or-path))
          property (validation/ensure string? (rt/->clj rt lua-property))
          node-id (node-id-or-path->node-id node-id-or-path project evaluation-context)]
      (some? (ext-value-getter node-id property evaluation-context)))))

(defn- mk-ext-can-set [project]
  (rt/lua-fn [lua-node-id-or-path lua-property]
    (let [{:keys [rt evaluation-context]} (rt/current-execution-context)
          node-id-or-path (validation/ensure ::validation/node-id (rt/->clj rt lua-node-id-or-path))
          property (validation/ensure string? (rt/->clj rt lua-property))
          node-id (node-id-or-path->node-id node-id-or-path project evaluation-context)]
      (some? (ext-value-setter node-id property project evaluation-context)))))

(defn- mk-ext-create-directory [project reload-resources]
  (rt/suspendable-lua-fn [lua-proj-path]
    (let [{:keys [rt evaluation-context]} (rt/current-execution-context)
          ^String proj-path (rt/->clj rt lua-proj-path)]
      (validation/ensure validation/resource-path? proj-path)
      (let [root-path (-> project
                          (project/workspace evaluation-context)
                          (workspace/project-path evaluation-context)
                          (fs/as-path)
                          (fs/to-real-path))
            dir-path (-> (str root-path proj-path)
                         (fs/as-path)
                         (.normalize))]
        (if (.startsWith dir-path root-path)
          (try
            (fs/create-path-directories! dir-path)
            (reload-resources)
            (catch FileAlreadyExistsException e
              (throw (LuaError. (str "File already exists: " (.getMessage e)))))
            (catch Exception e
              (throw (LuaError. (str (.getMessage e))))))
          (throw (LuaError. (str "Can't create " dir-path ": outside of project directory"))))))))

(defn- mk-ext-delete-directory [project reload-resources]
  (rt/suspendable-lua-fn [lua-proj-path]
    (let [{:keys [rt evaluation-context]} (rt/current-execution-context)
          proj-path (validation/ensure validation/resource-path? (rt/->clj rt lua-proj-path))
          root-path (-> project
                        (project/workspace evaluation-context)
                        (workspace/project-path evaluation-context)
                        (fs/as-path)
                        (fs/to-real-path))
          dir-path (-> (str root-path proj-path)
                       (fs/as-path)
                       (.normalize))
          protected-paths (mapv #(.resolve root-path ^String %)
                                [".git"
                                 ".internal"])
          protected-path? (fn protected-path? [^Path path]
                            (some #(.startsWith path ^Path %)
                                  protected-paths))]
      (cond
        (not (.startsWith dir-path root-path))
        (throw (LuaError. (str "Can't delete " dir-path ": outside of project directory")))

        (= (.getNameCount dir-path) (.getNameCount root-path))
        (throw (LuaError. (str "Can't delete the project directory itself")))

        (protected-path? dir-path)
        (throw (LuaError. (str "Can't delete " dir-path ": protected by editor")))

        :else
        (try
          (when (fs/delete-path-directory! dir-path)
            (reload-resources))
          (catch NotDirectoryException e
            (throw (LuaError. (str "Not a directory: " (.getMessage e)))))
          (catch Exception e
            (throw (LuaError. (str (.getMessage e))))))))))

(defn- ensure-file-path-in-project-directory
  ^Path [^String file-name project evaluation-context]
  (let [project-path (-> project
                         (project/workspace evaluation-context)
                         (workspace/project-path evaluation-context)
                         .toPath
                         .normalize)
        normalized-path (-> project-path
                            (.resolve file-name)
                            .normalize)]
    (if (.startsWith normalized-path project-path)
      normalized-path
      (throw (LuaError. (str "Can't access "
                             file-name
                             ": outside of project directory"))))))

(defn- mk-ext-remove-file [project reload-resources]
  (rt/suspendable-lua-fn [lua-file-name]
    (let [{:keys [rt evaluation-context]} (rt/current-execution-context)
          file-name (validation/ensure string? (rt/->clj rt lua-file-name))
          file-path (ensure-file-path-in-project-directory file-name project evaluation-context)]
      (when-not (Files/exists file-path (into-array LinkOption []))
        (throw (LuaError. (str "No such file or directory: " file-name))))
      (when-not (Files/delete file-path)
        (throw (LuaError. (str "Failed to delete " file-name))))
      (reload-resources))))

;; endregion

;; endregion

;; region language servers

(defn- built-in-language-servers []
  (let [lua-lsp-root (str (system/defold-unpack-path) "/" (.getPair (Platform/getHostPlatform)) "/bin/lsp/lua")]
    #{{:languages #{"lua"}
       :watched-files [{:pattern "**/.luacheckrc"}]
       :launcher {:command [(str lua-lsp-root "/bin/lua-language-server" (when (util/is-win32?) ".exe"))
                            (str "--configpath=" lua-lsp-root "/config.json")]}}}))

(defn- reload-language-servers! [project state evaluation-context]
  (let [lsp (lsp/get-node-lsp project)]
    (lsp/set-servers!
      lsp
      (into
        (built-in-language-servers)
        (comp
          (mapcat
            (fn [[path language-servers]]
              (try-with-extension-exceptions
                :display-output (:display-output state)
                :label (str "Reloading language servers in " path)
                :catch []
                (validation/ensure ::validation/language-servers language-servers))))
          (map (fn [language-server]
                 (-> language-server
                     (update :languages set)
                     (dissoc :command)
                     (assoc :launcher (select-keys language-server [:command]))))))
        (execute-all-top-level-functions state :get_language_servers {} evaluation-context)))))

;; endregion

;; region commands

;; region command action execution

(defmulti action->batched-executor+input (fn [action _project _evaluation-context]
                                           (:action action)))

(defn- transact! [txs _project _state]
  (let [f (future/make)]
    (fx/on-fx-thread
      (try
        (g/transact txs)
        (future/complete! f nil)
        (catch Exception ex (future/fail! f ex))))
    f))

(defmethod action->batched-executor+input "set" [action project evaluation-context]
  (let [node-id (node-id-or-path->node-id (:node_id action) project evaluation-context)
        property (:property action)
        setter (ext-value-setter node-id property project evaluation-context)]
    (if setter
      [transact! (setter (:value action))]
      (throw (LuaError.
               (format "Can't set \"%s\" property of %s"
                       property
                       (name (node-id->type-keyword node-id evaluation-context))))))))

(defn- await-all-sequentially
  "Sequentially execute a collection of functions that return CompletableFutures

  Returns a CompletableFuture with the result returned by the last future. Stops
  processing on first failed CompletableFuture"
  [future-fns]
  (reduce (fn [acc future-fn]
            (future/then-async acc (fn [_] (future-fn))))
          (future/completed nil)
          future-fns))

(defn- input-stream->console [input-stream display-output type]
  (future
    (error-reporting/catch-all!
      (with-open [reader (io/reader input-stream)]
        (doseq [line (line-seq reader)]
          (display-output type line))))))

(defn- shell! [commands project state]
  (let [{:keys [can-execute? reload-resources display-output]} state
        root (lsp.async/with-auto-evaluation-context evaluation-context
               (-> project
                   (project/workspace evaluation-context)
                   (workspace/project-path evaluation-context)))]
    (-> (await-all-sequentially
          (eduction
            (map
              (fn [cmd+args]
                #(future/then
                   (can-execute? cmd+args)
                   (fn [can-execute]
                     (if can-execute
                       (let [process (doto (apply process/start! {:dir root} cmd+args)
                                       (-> process/out (input-stream->console display-output :out))
                                       (-> process/err (input-stream->console display-output :err)))
                             exit-code (process/await-exit-code process)]
                         (when-not (zero? exit-code)
                           (throw (ex-info (str "Command \""
                                                (string/join " " cmd+args)
                                                "\" exited with code "
                                                exit-code)
                                           {:cmd cmd+args
                                            :exit-code exit-code}))))
                       (throw (ex-info (str "Command \"" (string/join " " cmd+args) "\" aborted") {:cmd cmd+args})))))))
            commands))
        (future/then (fn [_] (reload-resources))))))

(defmethod action->batched-executor+input "shell" [action _ _]
  [shell! (:command action)])

(defn- perform-actions! [actions project state evaluation-context]
  (await-all-sequentially
    (eduction (map #(action->batched-executor+input % project evaluation-context))
              (partition-by first)
              (map (juxt ffirst #(mapv second %)))
              (map (fn [[executor inputs]]
                     #(executor inputs project state)))
              actions)))

;; endregion

;; region command query compilation

(defn- continue [acc env lua-fn f & args]
  (let [new-lua-fn (fn [env m]
                     (lua-fn env (apply f m args)))]
    ((acc new-lua-fn) env)))

(defmacro gen-query [acc-sym [env-sym cont-sym] & body-expr]
  `(fn [lua-fn#]
     (fn [~env-sym]
       (let [~cont-sym (partial continue ~acc-sym ~env-sym lua-fn#)]
         ~@body-expr))))

(defmulti gen-selection-query (fn [q _acc _project]
                                (:type q)))

(defn- ensure-selection-cardinality [selection q]
  (if (= "one" (:cardinality q))
    (when (= 1 (count selection))
      (first selection))
    selection))

(defn- node-ids->lua-selection [selection q]
  (ensure-selection-cardinality (mapv vm/wrap-userdata selection) q))

(defmethod gen-selection-query "resource" [q acc project]
  (gen-query acc [env cont]
    (let [evaluation-context (or (:evaluation-context env) (g/make-evaluation-context))
          selection (:selection env)]
      (when-let [res (or (some-> selection
                                 (handler/adapt-every
                                   resource/ResourceNode
                                   #(-> %
                                        (g/node-value :resource evaluation-context)
                                        resource/proj-path
                                        some?))
                                 (node-ids->lua-selection q))
                         (some-> selection
                                 (handler/adapt-every resource/Resource)
                                 (->> (keep #(project/get-resource-node project % evaluation-context)))
                                 (node-ids->lua-selection q)))]
        (cont assoc :selection res)))))

(defmethod gen-selection-query "outline" [q acc _]
  (gen-query acc [env cont]
    (when-let [res (some-> (:selection env)
                           (handler/adapt-every outline/OutlineNode)
                           (node-ids->lua-selection q))]
      (cont assoc :selection res))))

(defn- compile-query [q project]
  (reduce-kv
    (fn [acc k v]
      (case k
        :selection (gen-selection-query v acc project)
        acc))
    (fn [lua-fn]
      (fn [env]
        (lua-fn env {})))
    q))

;; endregion

(defn- command->dynamic-handler [{:keys [label query active run locations]} path project state]
  (let [{:keys [rt display-output]} state
        lua-fn->env-fn (compile-query query project)
        contexts (into #{}
                       (map {"Assets" :asset-browser
                             "Outline" :outline
                             "Edit" :global
                             "View" :global})
                       locations)
        locations (into #{}
                        (map {"Assets" :editor.asset-browser/context-menu-end
                              "Outline" :editor.outline-view/context-menu-end
                              "Edit" :editor.app-view/edit-end
                              "View" :editor.app-view/view-end})
                        locations)]
    {:context-definition contexts
     :menu-item {:label label}
     :locations locations
     :fns (cond-> {}
                  active
                  (assoc :active?
                         (lua-fn->env-fn
                           (fn [env opts]
                             (try-with-extension-exceptions
                               :display-output display-output
                               :label (str label "'s \"active\" in " path)
                               :catch false
                               (rt/->clj rt (rt/invoke-immediate (:rt state) active (rt/->lua opts) (:evaluation-context env)))))))

                  (and (not active) query)
                  (assoc :active? (lua-fn->env-fn (constantly true)))

                  run
                  (assoc :run
                         (lua-fn->env-fn
                           (fn [_ opts]
                             (let [error-label (str label "'s \"run\" in " path)]
                               (-> (rt/invoke-suspending rt run (rt/->lua opts))
                                   (future/then
                                     (fn [lua-result]
                                       (when-let [actions (rt/->clj rt lua-result)]
                                         (lsp.async/with-auto-evaluation-context evaluation-context
                                           (perform-actions! (validation/ensure ::validation/actions actions)
                                                             project
                                                             state
                                                             evaluation-context)))))
                                   (future/catch #(display-script-error display-output error-label %))))))))}))

(defn- reload-commands! [project state evaluation-context]
  (let [{:keys [display-output]} state]
    (handler/register-dynamic! ::commands
      (into []
            (mapcat
              (fn [[path ret]]
                (try-with-extension-exceptions
                  :display-output display-output
                  :label (str "Reloading commands in " path)
                  :catch nil
                  (eduction
                    (keep (fn [command]
                            (try-with-extension-exceptions
                              :display-output display-output
                              :label (str (:label command) " in " path)
                              :catch nil
                              (command->dynamic-handler command path project state))))
                    (validation/ensure ::validation/commands ret)))))
            (execute-all-top-level-functions state :get_commands {} evaluation-context)))))

;; endregion

;; region reload

(defn- add-all-entry [m path module]
  (reduce-kv
    (fn [acc k v]
      (update acc k (fnil conj []) [path v]))
    m
    module))

(def hooks-file-path "/hooks.editor_script")

(defn- re-create-ext-state [initial-state evaluation-context]
  (let [{:keys [rt display-output]} initial-state]
    (->> [:library-prototypes :project-prototypes]
         (eduction (mapcat initial-state))
         (reduce
           (fn [acc x]
             (cond
               (instance? LuaError x)
               (do
                 (display-output :err (str "Compilation failed" (some->> (ex-message x) (str ": "))))
                 acc)

               (instance? Prototype x)
               (let [proto-path (.tojstring (.-source ^Prototype x))]
                 (if-let [module (try-with-extension-exceptions
                                   :display-output display-output
                                   :label (str "Loading " proto-path)
                                   :catch nil
                                   (validation/ensure ::validation/module
                                     (rt/->clj rt (rt/invoke-immediate rt (rt/bind rt x) evaluation-context))))]
                   (-> acc
                       (update :all add-all-entry proto-path module)
                       (cond-> (= hooks-file-path proto-path)
                               (assoc :hooks module)))
                   acc))

               (nil? x)
               acc

               :else
               (throw (ex-info (str "Unexpected prototype value: " x) {:prototype x}))))
           initial-state))))

(defn line-writer [f]
  (let [sb (StringBuilder.)]
    (PrintWriter-on #(doseq [^char ch %]
                       (if (= \newline ch)
                         (let [str (.toString sb)]
                           (.delete sb 0 (.length sb))
                           (f str))
                         (.append sb ch)))
                    nil)))

;; endregion

;; region public API

(defn reload!
  "Reload the extensions

  Args:
    project    the project node id
    kind       which scripts to reload, either :all, :library or :project

  Required kv-args:
    :reload-resources    0-arg function that asynchronously reloads the editor
                         resources, returns a CompletableFuture (that might
                         complete exceptionally if reload fails)
    :can-execute?        1-arg function that takes in a command vector and asks
                         the user if it's okay to execute this command. Returns
                         a CompletableFuture that will resolve to boolean
    :display-output      2-arg function used for displaying output in the
                         console, the args are:
                           type    output type, :out or :err
                           msg     a string to output, might be multiline"
  [project kind & {:keys [reload-resources can-execute? display-output] :as opts}]
  (g/with-auto-evaluation-context evaluation-context
    (let [extensions (g/node-value project :editor-extensions evaluation-context)
          old-state (ext-state project evaluation-context)
          new-state (re-create-ext-state
                      (assoc opts
                        :rt (rt/make project
                                     :out (line-writer #(display-output :out %))
                                     :err (line-writer #(display-output :err %))
                                     :env {"editor" {"get" (mk-ext-get project)
                                                     "can_get" (mk-ext-can-get project)
                                                     "can_set" (mk-ext-can-set project)
                                                     "create_directory" (mk-ext-create-directory project reload-resources)
                                                     "delete_directory" (mk-ext-delete-directory project reload-resources)
                                                     "platform" (.getPair (Platform/getHostPlatform))
                                                     "version" (system/defold-version)
                                                     "engine_sha1" (system/defold-engine-sha1)
                                                     "editor_sha1" (system/defold-editor-sha1)}
                                           "io" {"tmpfile" nil}
                                           "os" {"execute" nil
                                                 "exit" nil
                                                 "remove" (mk-ext-remove-file project reload-resources)
                                                 "rename" nil
                                                 "setlocale" nil
                                                 "tmpname" nil}})
                        :library-prototypes (if (or (= :all kind) (= :library kind))
                                              (g/node-value extensions :library-prototypes evaluation-context)
                                              (:library-prototypes old-state []))
                        :project-prototypes (if (or (= :all kind) (= :project kind))
                                              (g/node-value extensions :project-prototypes evaluation-context)
                                              (:project-prototypes old-state [])))
                      evaluation-context)]
      (g/user-data-swap! extensions :state (constantly new-state))
      (reload-language-servers! project new-state evaluation-context)
      (reload-commands! project new-state evaluation-context))))

(defn hook-exception->error [^Throwable ex project hook-keyword]
  (let [^Throwable root (stacktrace/root-cause ex)
        message (ex-message root)
        [_ file line :as match] (re-find console/line-sub-regions-pattern message)]
    (g/map->error
      (cond-> {:_node-id (or (when match (project/get-resource-node project file))
                             (project/get-resource-node project hooks-file-path))
               :message (str (name hook-keyword) " in " hooks-file-path " failed: " message)
               :severity :fatal}

              line
              (assoc-in [:user-data :cursor-range]
                        (data/line-number->CursorRange (Integer/parseInt line)))))))

(defn execute-hook!
  "Execute hook defined in this project

  Returns a CompletableFuture that will finish when the hook processing is
  finished. If the hook execution fails, the error will be reported to the user
  and the future will be completed exceptionally.

  Args:
    project         the project node id
    hook-keyword    keyword like :on_build_started
    opts            an object that will be serialized and passed to the Lua
                    hook function. WARNING: all node ids should be wrapped with
                    vm/wrap-user-data since Lua numbers lack necessary precision"
  [project hook-keyword opts]
  (g/with-auto-evaluation-context evaluation-context
    (let [{:keys [rt display-output hooks] :as state} (ext-state project evaluation-context)]
      (if-let [lua-fn (get hooks hook-keyword)]
        (-> (rt/invoke-suspending rt lua-fn (rt/->lua opts))
            (future/then
              (fn [lua-result]
                (when-let [actions (rt/->clj rt lua-result)]
                  (lsp.async/with-auto-evaluation-context evaluation-context
                    (perform-actions! (validation/ensure ::validation/actions actions)
                                      project
                                      state
                                      evaluation-context)))))
            (future/catch
              (fn [ex]
                (display-script-error display-output (str "hook " (name hook-keyword)) ex)
                (throw ex))))
        (future/completed nil)))))

;; endregion

;; TODO remove project dependency in runtime/make!
;; todo tests
;; todo test that requires in editor scripts work, including on project start
;; todo make file:write and(or?) file:flush suspendable
;; todo test language servers
;; todo test outline properties and converters
