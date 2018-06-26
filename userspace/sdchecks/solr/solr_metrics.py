import json
import logging
import urllib2
import socket

from urlparse import urlparse
from enum import Enum
from utils.network import Network


class SolrMetrics(object):

    class Tag(Enum):
        COLLECTION = 1
        SHARD = 2
        CORE = 3
        CORE_ALIAS = 4

    TAG_NAME = {
        Tag.COLLECTION: "solr.collection.name:%s",
        Tag.SHARD: "solr.shard.name:%s",
        Tag.CORE: "solr.core.name:%s",
        Tag.CORE_ALIAS: "solr.core.alias:%s",
    }

    class METRIC_NAME_ENUM(Enum):
        LIVE_NODES = 1,
        SHARDS = 2,
        REPLICA = 3,
        DOCUMENT_COUNT = 4,
        DOCUMENT_COUNT_MAX = 5,
        DOCUMENT_COUNT_DELETED = 6,
        BROWSE_RPS = 7,
        SELECT_RPS = 8,
        GET_RPS = 9,
        QUERY_RPS = 10,
        UPDATE_RPS = 11,
        INDEX_SIZE_REP = 12,
        BROWSE_RT = 13,
        SELECT_RT = 14,
        GET_RT = 15,
        QUERY_RT = 16,
        UPDATE_RT = 17,
        HOST_SHARD_COUNT = 18,
        COLLECTION_SHARD_COUNT = 19,
        UPDATEHANDLER_ADDS = 20,
        UPDATEHANDLER_DELETES_BY_ID = 21,
        UPDATEHANDLER_DELETES_BY_QUERY = 22,
        UPDATEHANDLER_COMMITS = 23,
        UPDATEHANDLER_AUTOCOMMITS = 24,
        INDEX_SIZE_LOG = 25,
        NONE = 100

    class Endpoint(Enum):
        LIVE_NODES = 1
        SHARDS = 2
        REPLICA = 3
        DOCUMENT_COUNT = 4
        COLLECTION = 5
        NODE = 6
        CORES_INFO = 7
        VERSION = 8
        STATS = 9

    URL = {
        Endpoint.LIVE_NODES:        "/solr/admin/collections?action=clusterstatus&wt=json",
        Endpoint.SHARDS:            "/solr/admin/collections?action=clusterstatus&wt=json",
        Endpoint.REPLICA:           "/solr/admin/collections?action=clusterstatus&wt=json",
        Endpoint.DOCUMENT_COUNT:    "/solr/admin/cores?wt=json",
        Endpoint.COLLECTION:        "/solr/admin/collections?action=clusterstatus&wt=json",
    }

    class Metric:

        class MetricType(Enum):
            gauge = 1
            counter = 2
            rate = 3

        def __init__(self, name, value, tags, metricType = MetricType.gauge):
            self.name = name
            self.value = value
            self.tags = tags
            self.metricType = metricType

        def getValue(self):
            return self.value

        def getTags(self):
            return self.tags

        def getName(self):
            return self.name

        def getType(self):
            return self.metricType

        def __repr__(self):
            return ("(name:{}, value: {}. tags: {})").format(self.name, self.value, self.tags)

        def __str__(self):
            return ("(name:{}, value: {}. tags: {})").format(self.name, self.value, self.tags)

    class Core:
        def __init__(self, name, alias, shard, collection, base_url, port, leader = None):
            self.name = name
            self.alias = alias
            self.shard = shard
            self.collection = collection
            self.base_url = base_url
            self.port = port
            self.leader = leader

        def __hash__(self):
            return ("{}{}{}{}{}").format(self.name, self.alias, self.shard, self.collection, self.base_url).__hash__()

        def __eq__(self, other):
            return self.name == other.name and self.alias == other.alias and self.shard == other.shard and self.collection == other.collection and self.base_url == other.base_url

        def getPort(self):
            return self.port

    def __init__(self, version, instance):
        self.version = version
        self.instance = instance
        self.ports = instance["ports"]
        self.port = 0
        self.host = instance["host"]
        self.network = Network()
        self.localCores = set()
        self.localLeaderCores = set()
        self.localEndpoints = set()
        self.collectionByCore = dict()
        self.log = logging.getLogger(__name__)

    def check(self):
        self.localCores = set()
        self.localLeaderCores = set()
        self.localEndpoints = set()
        self.collectionByCore = dict()

        self.log.debug(str("solr: Start metrics collection: host {}, port {}, ports {}").format(self.host, self.port, self.ports))
        self._retrieveLocalEndpointsAndCores()
        ret = [
            self._getLiveNodes(),
            self._getReplica(),
            self._getLocalDocumentCount(),
            self._getAllRpsAndRequestTime(),
            self._getIndexSize(),
            self._getCollectionShardCount(),
            self._getHostShardCount()
        ]
        self.log.debug(str("solr: End metrics collection"))
        return ret

    def getMajorNumberVersion(self):
        return int(self.version[0:1])

    @staticmethod
    def formatUrl(host, port, handler):
        ret = "http://" + host + ":" + str(port) + handler
        return ret

    @staticmethod
    def getUrl(host, ports, handler):
        found = False
        foundPort = 0
        for port in ports:
            try:
                if found is True:
                    break
                url = SolrMetrics.formatUrl(host, port, handler)
                data = urllib2.urlopen(url)
                obj = json.load(data)
                found = True
                foundPort = port
            except:
                found = False
        if found is True:
            return [obj, foundPort]
        else:
            return [{}, 0]

    def _getUrl(self, handler):
        ports = [ self.port ] if self.port else self.ports
        obj, self.port = SolrMetrics.getUrl(self.host, ports, handler)
        return obj

    def _getUrlWithBase(self, baseUrl, handler):
        url = str("{}{}").format(baseUrl[0:baseUrl.find('/solr')], handler)
        try:
            data = urllib2.urlopen(url)
            obj = json.load(data)
        except:
            return {}

        return obj

    def _isLocal(self, ip, port):
        try:
            ret = self.network.ipIsLocalHostOrDockerContainer(ip) and int(self.port) == int(port)
            self.log.debug("{}:{} is {} to {}:{}".format(ip, port, "local" if ret else "not local", "localhost", self.port))
        except:
            self.log.warning("Failed to determine locality of {}:{} on {}:{}".format(ip, port, "localhost", self.port))
            return False

        return ret

    def _getLiveNodes(self):
        ret = []
        obj = self._getUrl(SolrMetrics.URL[SolrMetrics.Endpoint.LIVE_NODES])

        if len(obj) > 0:
            try:
                live_node_count = 0
                for live_node in obj["cluster"]["live_nodes"]:
                    hostname = live_node.split(':')[0]
                    port = live_node.split(':')[1].split('_')[0]
                    ip_address = socket.gethostbyname(hostname)
                    if self._isLocal(ip_address, port):
                        live_node_count += 1

                ret.append(self.Metric(self.METRIC_NAME_ENUM.LIVE_NODES, live_node_count, None))
                self.log.debug(("detected {} live local nodes").format(live_node_count))
            except KeyError:
                pass
        return ret

    def _getCollectionShardCount(self):
        ret = []
        try:
            obj = self._getUrl(SolrMetrics.URL[SolrMetrics.Endpoint.SHARDS])
            if len(obj) == 0:
                return ret

            for collection in obj["cluster"]["collections"]:
                shards_per_collection = len(obj["cluster"]["collections"][collection]["shards"])
                tags = [ self.TAG_NAME[self.Tag.COLLECTION] % collection ]
                ret.append(self.Metric(self.METRIC_NAME_ENUM.COLLECTION_SHARD_COUNT, shards_per_collection, tags))
        except Exception as e:
            self.log.error(("Error while fetching collection shard count: {}").format(e))
        return ret

    def _getHostShardCount(self):
        ret = []
        try:
            obj = self._getUrl(SolrMetrics.URL[SolrMetrics.Endpoint.SHARDS])
            if len(obj) == 0:
                return ret

            for collection in obj["cluster"]["collections"]:
                shards_per_host = 0
                shards = obj["cluster"]["collections"][collection]["shards"]

                for shard in shards.values():
                    for replica in shard["replicas"].values():
                        base_url = replica["base_url"]
                        parsedUrl = urlparse(base_url)
                        node_name = parsedUrl.hostname
                        port = parsedUrl.port
                        node_ip_address = socket.gethostbyname(node_name)
                        if self._isLocal(node_ip_address, port):
                            # found a replica that is local to this host
                            shards_per_host = shards_per_host + 1
                            break

                tags = [ self.TAG_NAME[self.Tag.COLLECTION] % collection ]
                ret.append(self.Metric(self.METRIC_NAME_ENUM.HOST_SHARD_COUNT, shards_per_host, tags))
        except Exception as e:
            self.log.error(("Error while fetching host shard count: {}").format(e))
        return ret

    def _getReplica(self):
        class replicaPerNode:
            pass

        ret = []
        try:
            obj = self._getUrl(SolrMetrics.URL[SolrMetrics.Endpoint.REPLICA])
            if len(obj) > 0:
                for collectionName, collection in obj["cluster"]["collections"].iteritems():
                    replicaCount = 0
                    for shardName, shard in collection["shards"].iteritems():
                        for coreAlias, replica in shard["replicas"].iteritems():
                            if replica["state"] == "active":
                                nodeName = replica["node_name"]
                                coreName = replica["core"]
                                baseUrl = replica["base_url"]
                                thisCore = self.Core(coreName, coreAlias, shardName, collectionName, baseUrl, urlparse(baseUrl).port)
                                if thisCore in self.localCores:
                                    replicaCount += 1
                    if replicaCount > 0:
                        tags = [
                            self.TAG_NAME[self.Tag.COLLECTION] % collectionName,
                        ]
                        ret.append(self.Metric(self.METRIC_NAME_ENUM.REPLICA, replicaCount, tags))
                        self.log.debug(("detected {} replica with tags {}").format(replicaCount, tags))
        except Exception as e:
            self.log.error(("Error while fetching replica: {}").format(e))

        return ret

    def _getLocalDocumentCount(self):
        ret = []
        for base in self.localEndpoints:
            mets = self._getCoreDocumentCount(base)
            ret.extend(mets)
        return ret

    def _getCoreDocumentCount(self, base):
        ret = []
        try:
            obj = self._getUrlWithBase(base, SolrMetrics.URL[SolrMetrics.Endpoint.DOCUMENT_COUNT])
            if len(obj) > 0:
                for core_name in obj["status"]:
                    tags = [
                        self.TAG_NAME[self.Tag.CORE] % core_name
                    ]
                    collectionName = self.collectionByCore.get(core_name, None)
                    if collectionName is not None:
                        tags.append(self.TAG_NAME[self.Tag.COLLECTION] % collectionName)

                    if core_name not in self.localLeaderCores:
                        # Report 0 for non-leader cores so that the number panel in the host specific dashboard has data to show
                        ret.append(self.Metric(self.METRIC_NAME_ENUM.DOCUMENT_COUNT, 0, tags))
                        ret.append(self.Metric(self.METRIC_NAME_ENUM.DOCUMENT_COUNT_MAX, 0, tags))
                        ret.append(self.Metric(self.METRIC_NAME_ENUM.DOCUMENT_COUNT_DELETED, 0, tags))
                        continue

                    numDocs = obj["status"][core_name]["index"]["numDocs"]
                    maxDoc = obj["status"][core_name]["index"]["maxDoc"]
                    deletedDocs = obj["status"][core_name]["index"]["deletedDocs"]

                    ret.append(self.Metric(self.METRIC_NAME_ENUM.DOCUMENT_COUNT, numDocs, tags))
                    ret.append(self.Metric(self.METRIC_NAME_ENUM.DOCUMENT_COUNT_MAX, maxDoc, tags))
                    ret.append(self.Metric(self.METRIC_NAME_ENUM.DOCUMENT_COUNT_DELETED, deletedDocs, tags))
        except Exception as e:
            self.log.error(("Error while fetching core document count: {}").format(e))
        return ret

    def _retrieveLocalEndpointsAndCores(self):
        obj = self._getUrl(SolrMetrics.URL[SolrMetrics.Endpoint.LIVE_NODES])
        if len(obj) > 0:
            try:
                for collectionName, collection in obj["cluster"]["collections"].iteritems():
                    for shardName, shard in collection["shards"].iteritems():
                        for coreAlias, replica in shard["replicas"].iteritems():
                            if replica["state"] != "active":
                                self.log.debug(("Skipping core {}_{}_{} in state {} on node {}").format(collectionName, shardName, coreAlias, replica["state"], replica["base_url"]))
                                continue
                            base_url = replica["base_url"]
                            parsedUrl = urlparse(base_url)
                            hostname_from_url = parsedUrl.hostname
                            port_from_url = parsedUrl.port
                            ip_address = socket.gethostbyname(hostname_from_url)
                            if self._isLocal(ip_address, port_from_url):
                                coreName = replica["core"]

                                leader = replica.get("leader", False)
                                if bool(leader):
                                    self.localLeaderCores.add(coreName)

                                self.collectionByCore[coreName] = collectionName
                                self.localCores.add(self.Core(coreName, coreAlias, shardName, collectionName, base_url, port_from_url))
                                self.localEndpoints.add(base_url)
                                self.log.debug(str("detected local core {}:{} on node {}").format(coreName, coreAlias, base_url))
            except Exception as e:
                self.log.error(("Error while attempting to fetch local cores: {}").format(e))

    def _getCollections(self):
        ret = []
        obj = self._getUrl(SolrMetrics.URL[SolrMetrics.Endpoint.COLLECTION])
        if len(obj) > 0:
            for collection in obj["cluster"]["collections"]:
                ret.append(collection)
        return ret
